#include "QemuBridge.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

extern "C" {
#include "dbus-display1.h"
}

extern char** environ;

// ─── GLib/GDBus state (kept out of the header) ───────────────────────────
struct QemuBridge::Impl {
    QemuBridge* owner = nullptr;

    GMainContext* ctx = nullptr;
    GMainLoop* loop = nullptr;

    int mainFd = -1;                 // our end of the p2p bus socket (client)
    GDBusConnection* mainConn = nullptr;
    GDBusConnection* listenerConn = nullptr;

    QemuDBusDisplay1Console* console = nullptr;
    QemuDBusDisplay1Keyboard* keyboard = nullptr;
    QemuDBusDisplay1Mouse* mouse = nullptr;
    bool mouseIsAbs = true;

    QemuDBusDisplay1Clipboard* clipIface = nullptr;   // our skeleton
    QemuDBusDisplay1Clipboard* clipProxy = nullptr;   // proxy to QEMU's

    GDBusObjectManagerServer* server = nullptr;
    QemuDBusDisplay1Listener* listenerIface = nullptr;
    QemuDBusDisplay1ListenerUnixMap* mapIface = nullptr;

    // Unix.Map shared-memory scanout (fast path). Mapping owned by D-Bus thread.
    void* mapAddr = nullptr;         // mmap base (page-aligned; for munmap)
    size_t mapLen = 0;
    const uint8_t* mapData = nullptr;  // surface start (base + intra-page offset)
    uint32_t mapW = 0, mapH = 0, mapStride = 0;

    // Listener-registration handshake bookkeeping.
    GThread* listenerConnThread = nullptr;
    GUnixFDList* regFdList = nullptr;
    int listenerLocalFd = -1;        // our end of the listener socket
};

// ════════════════════════════════════════════════════════════════════════
//  QMP: launch QEMU and hand it our bus socket via getfd + add_client.
//  (Mirrors qtest_qmp_add_client in qemu/tests/qtest/dbus-display-test.c.)
// ════════════════════════════════════════════════════════════════════════
namespace {

// Read one '\n'-terminated line from a QMP socket, buffering the remainder.
bool qmpReadLine(int fd, std::string& buf, std::string& line) {
    for (;;) {
        auto nl = buf.find('\n');
        if (nl != std::string::npos) {
            line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            return true;
        }
        char tmp[4096];
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n <= 0) return false;
        buf.append(tmp, n);
    }
}

// Consume lines until we see a command reply ("return" or "error"), skipping
// asynchronous events and the greeting. Returns false on an error reply/EOF.
//
// Used only for the handshake, where no id has been assigned yet.
bool qmpWaitReturn(int fd, std::string& buf) {
    std::string line;
    while (qmpReadLine(fd, buf, line)) {
        if (line.find("\"error\"") != std::string::npos) {
            std::fprintf(stderr, "QMP error: %s\n", line.c_str());
            return false;
        }
        if (line.find("\"return\"") != std::string::npos) return true;
        // else: greeting ("QMP") or event ("event") — keep reading.
    }
    return false;
}

// Wait for the reply carrying exactly `idTag`, skipping asynchronous events.
//
// Matching on a bare "return"/"error" substring was wrong twice over: QEMU
// emits spontaneous events (RESET, STOP, DEVICE_TRAY_MOVED…) that can carry
// those words, and a filename passed to blockdev-change-medium containing
// "error" was enough to self-trap. Replies are correlated by id; events have
// none, so they are simply skipped.
//
// `*fatal` is set when the channel can no longer be trusted (EOF or a read
// timeout leaving a partial line in `buf`): the caller closes it rather than
// reading desynchronised data forever after.
bool qmpWaitId(int fd, std::string& buf, const std::string& idTag, bool* fatal) {
    std::string line;
    *fatal = false;
    while (qmpReadLine(fd, buf, line)) {
        if (line.find(idTag) == std::string::npos) continue;   // event or other cmd
        if (line.find("\"error\"") != std::string::npos) {
            std::fprintf(stderr, "QMP error: %s\n", line.c_str());
            return false;
        }
        return line.find("\"return\"") != std::string::npos;
    }
    *fatal = true;   // EOF or timeout: `buf` may hold half a line
    return false;
}

bool qmpSend(int fd, const std::string& json) {
    std::string line = json + "\n";
    return write(fd, line.data(), line.size()) == (ssize_t)line.size();
}

// Send a QMP command carrying one file descriptor via SCM_RIGHTS (getfd).
bool qmpSendWithFd(int fd, const std::string& json, int passfd) {
    std::string line = json + "\n";
    struct iovec io {};
    io.iov_base = (void*)line.data();
    io.iov_len = line.size();

    char cbuf[CMSG_SPACE(sizeof(int))];
    std::memset(cbuf, 0, sizeof cbuf);
    struct msghdr msg {};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof cbuf;
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(c), &passfd, sizeof(int));
    return sendmsg(fd, &msg, 0) >= 0;
}

int connectUnix(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════
//  Listener callbacks (run on the D-Bus thread) — pixels into framebuffer.
// ════════════════════════════════════════════════════════════════════════
static gboolean on_scanout(QemuDBusDisplay1Listener* obj,
                           GDBusMethodInvocation* inv, guint width,
                           guint height, guint stride, guint pixman_format,
                           GVariant* data, gpointer user_data) {
    auto* self = static_cast<QemuBridge*>(user_data);
    gsize len = g_variant_get_size(data);
    const auto* bytes = static_cast<const uint8_t*>(g_variant_get_data(data));
    self->ingestScanout(width, height, stride, pixman_format, bytes, len);
    qemu_dbus_display1_listener_complete_scanout(obj, inv);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean on_update(QemuDBusDisplay1Listener* obj,
                          GDBusMethodInvocation* inv, gint x, gint y, gint w,
                          gint h, guint stride, guint pixman_format,
                          GVariant* data, gpointer user_data) {
    auto* self = static_cast<QemuBridge*>(user_data);
    gsize len = g_variant_get_size(data);
    const auto* bytes = static_cast<const uint8_t*>(g_variant_get_data(data));
    self->ingestUpdate(x, y, w, h, stride, pixman_format, bytes, len);
    qemu_dbus_display1_listener_complete_update(obj, inv);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

// Unix.Map fast path: QEMU passes a shared-memory fd (ScanoutMap), then only
// tells us which region changed (UpdateMap) — no pixels cross the bus.
static gboolean on_scanout_map(QemuDBusDisplay1ListenerUnixMap* obj,
                               GDBusMethodInvocation* inv, GUnixFDList* fd_list,
                               GVariant* arg_handle, guint offset, guint width,
                               guint height, guint stride, guint pixman_format,
                               gpointer user_data) {
    auto* self = static_cast<QemuBridge*>(user_data);
    GError* err = nullptr;
    int fd = g_unix_fd_list_get(fd_list, g_variant_get_handle(arg_handle), &err);
    if (fd < 0) {
        std::fprintf(stderr, "ScanoutMap: no fd: %s\n", err ? err->message : "?");
        g_clear_error(&err);
    } else {
        self->mapScanout(fd, offset, width, height, stride, pixman_format);
    }
    qemu_dbus_display1_listener_unix_map_complete_scanout_map(obj, inv, nullptr);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean on_update_map(QemuDBusDisplay1ListenerUnixMap* obj,
                              GDBusMethodInvocation* inv, gint x, gint y, gint w,
                              gint h, gpointer user_data) {
    static_cast<QemuBridge*>(user_data)->mapUpdate(x, y, w, h);
    qemu_dbus_display1_listener_unix_map_complete_update_map(obj, inv);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

// Stubs so QEMU's fire-and-forget calls don't return D-Bus errors.
static gboolean on_disable(QemuDBusDisplay1Listener* o,
                           GDBusMethodInvocation* i, gpointer) {
    qemu_dbus_display1_listener_complete_disable(o, i);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
}
static gboolean on_mouse_set(QemuDBusDisplay1Listener* o,
                             GDBusMethodInvocation* i, gint, gint, gint,
                             gpointer) {
    qemu_dbus_display1_listener_complete_mouse_set(o, i);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
}
static gboolean on_cursor_define(QemuDBusDisplay1Listener* o,
                                 GDBusMethodInvocation* i, gint, gint, gint,
                                 gint, GVariant*, gpointer) {
    qemu_dbus_display1_listener_complete_cursor_define(o, i);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

// Build a p2p GDBus *client* connection from a connected fd (QEMU authenticates
// as the server). Runs on the caller's thread; binds to its thread-default
// GMainContext. Auth itself is driven by GDBus's own worker thread, so a
// synchronous build here does not deadlock the D-Bus main loop.
static GDBusConnection* p2p_client_from_fd(int fd) {
    GError* err = nullptr;
    GSocket* sock = g_socket_new_from_fd(fd, &err);
    if (!sock) {
        std::fprintf(stderr, "g_socket_new_from_fd: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        return nullptr;
    }
    GSocketConnection* sc = g_socket_connection_factory_create_connection(sock);
    g_object_unref(sock);
    GDBusConnection* conn = g_dbus_connection_new_sync(
        G_IO_STREAM(sc), nullptr,
        (GDBusConnectionFlags)(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                               G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING),
        nullptr, nullptr, &err);
    g_object_unref(sc);
    if (!conn) {
        std::fprintf(stderr, "listener connection: %s\n", err ? err->message : "?");
        g_clear_error(&err);
    }
    return conn;
}

static void on_listener_registered(GObject* src, GAsyncResult* res,
                                   gpointer user_data) {
    auto* self = static_cast<QemuBridge*>(user_data);
    QemuBridge::Impl* impl = self->impl();
    GError* err = nullptr;
    qemu_dbus_display1_console_call_register_listener_finish(
        QEMU_DBUS_DISPLAY1_CONSOLE(src), nullptr, res, &err);
    if (err) {
        std::fprintf(stderr, "RegisterListener failed: %s\n", err->message);
        g_clear_error(&err);
        return;
    }

    // QEMU is now serving the listener socket (its step after completing our
    // call) — build our client end on this (D-Bus) thread.
    impl->listenerConn = p2p_client_from_fd(impl->listenerLocalFd);
    impl->listenerLocalFd = -1;  // now owned by the GSocket/connection
    if (impl->regFdList) { g_object_unref(impl->regFdList); impl->regFdList = nullptr; }
    if (!impl->listenerConn) return;

    // Export the Listener object; QEMU pushes Scanout/Update onto it.
    impl->server = g_dbus_object_manager_server_new("/org/qemu/Display1");
    GDBusObjectSkeleton* obj =
        g_dbus_object_skeleton_new("/org/qemu/Display1/Listener");
    impl->listenerIface = qemu_dbus_display1_listener_skeleton_new();
    // Advertise the Unix.Map interface so QEMU prefers the shared-memory
    // scanout (an fd we mmap) over copying pixels through the bus. The
    // property must be a non-NULL strv or QEMU asserts in g_strv_contains();
    // if the surface has no share handle QEMU silently falls back to inline
    // Scanout/Update, which we also handle.
    {
        const gchar* ifaces[] = {"org.qemu.Display1.Listener.Unix.Map", nullptr};
        g_object_set(impl->listenerIface, "interfaces", ifaces, nullptr);
    }
    g_signal_connect(impl->listenerIface, "handle-scanout",
                     G_CALLBACK(on_scanout), self);
    g_signal_connect(impl->listenerIface, "handle-update",
                     G_CALLBACK(on_update), self);
    g_signal_connect(impl->listenerIface, "handle-disable",
                     G_CALLBACK(on_disable), self);
    g_signal_connect(impl->listenerIface, "handle-mouse-set",
                     G_CALLBACK(on_mouse_set), self);
    g_signal_connect(impl->listenerIface, "handle-cursor-define",
                     G_CALLBACK(on_cursor_define), self);
    g_dbus_object_skeleton_add_interface(
        obj, G_DBUS_INTERFACE_SKELETON(impl->listenerIface));

    // Unix.Map interface on the same object.
    impl->mapIface = qemu_dbus_display1_listener_unix_map_skeleton_new();
    g_signal_connect(impl->mapIface, "handle-scanout-map",
                     G_CALLBACK(on_scanout_map), self);
    g_signal_connect(impl->mapIface, "handle-update-map",
                     G_CALLBACK(on_update_map), self);
    g_dbus_object_skeleton_add_interface(
        obj, G_DBUS_INTERFACE_SKELETON(impl->mapIface));
    g_dbus_object_manager_server_export(impl->server, obj);
    g_object_unref(obj);
    g_dbus_object_manager_server_set_connection(impl->server,
                                                impl->listenerConn);
    g_dbus_connection_start_message_processing(impl->listenerConn);
    std::fprintf(stderr, "QemuBridge: listener registered, awaiting scanouts\n");
}

// ── Clipboard peer handlers (our skeleton; called by QEMU on the D-Bus thread) ─
static gboolean on_clip_register(QemuDBusDisplay1Clipboard* o,
                                 GDBusMethodInvocation* inv, gpointer u) {
    static_cast<QemuBridge*>(u)->clipResetSerial();
    qemu_dbus_display1_clipboard_complete_register(o, inv);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
}
static gboolean on_clip_unregister(QemuDBusDisplay1Clipboard* o,
                                   GDBusMethodInvocation* inv, gpointer) {
    qemu_dbus_display1_clipboard_complete_unregister(o, inv);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
}
static gboolean on_clip_release(QemuDBusDisplay1Clipboard* o,
                                GDBusMethodInvocation* inv, guint, gpointer) {
    qemu_dbus_display1_clipboard_complete_release(o, inv);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
}
// QEMU tells us the GUEST grabbed the clipboard → fetch its text for the host.
static gboolean on_clip_grab(QemuDBusDisplay1Clipboard* o,
                             GDBusMethodInvocation* inv, guint selection,
                             guint /*serial*/, const gchar* const* /*mimes*/,
                             gpointer u) {
    auto* self = static_cast<QemuBridge*>(u);
    QemuBridge::Impl* d = self->impl();
    if (selection == 0 && d->clipProxy) {
        const gchar* want[] = {"text/plain;charset=utf-8", "text/plain", nullptr};
        gchar* rmime = nullptr; GVariant* data = nullptr; GError* err = nullptr;
        if (qemu_dbus_display1_clipboard_call_request_sync(
                d->clipProxy, 0, want, G_DBUS_CALL_FLAGS_NONE, 2000, &rmime,
                &data, nullptr, &err)) {
            gsize n = g_variant_get_size(data);
            const char* bytes = static_cast<const char*>(g_variant_get_data(data));
            if (bytes && n) self->clipStoreGuest(std::string(bytes, n));
            std::fprintf(stderr, "QemuBridge: clipboard guest→host (%zu bytes)\n",
                         (size_t)n);
            g_variant_unref(data);
            g_free(rmime);
        } else {
            g_clear_error(&err);
        }
    }
    qemu_dbus_display1_clipboard_complete_grab(o, inv);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
}
// QEMU asks US for the clipboard (guest wants to paste the host's text).
static gboolean on_clip_request(QemuDBusDisplay1Clipboard* o,
                                GDBusMethodInvocation* inv, guint /*selection*/,
                                const gchar* const* /*mimes*/, gpointer u) {
    std::string text = static_cast<QemuBridge*>(u)->clipLocalText();
    GVariant* data = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, text.data(),
                                               text.size(), 1);
    qemu_dbus_display1_clipboard_complete_request(
        o, inv, "text/plain;charset=utf-8", data);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
}
static void on_clip_grab_done(GObject* src, GAsyncResult* res, gpointer) {
    GError* err = nullptr;
    qemu_dbus_display1_clipboard_call_grab_finish(
        QEMU_DBUS_DISPLAY1_CLIPBOARD(src), res, &err);
    if (err) {
        std::fprintf(stderr, "clipboard grab rejected: %s\n", err->message);
        g_clear_error(&err);
    } else {
        std::fprintf(stderr, "QemuBridge: clipboard grab accepted by QEMU\n");
    }
}
static void on_clip_registered(GObject* src, GAsyncResult* res, gpointer) {
    GError* err = nullptr;
    qemu_dbus_display1_clipboard_call_register_finish(
        QEMU_DBUS_DISPLAY1_CLIPBOARD(src), res, &err);
    if (err) {
        std::fprintf(stderr, "clipboard Register failed: %s\n", err->message);
        g_clear_error(&err);
    } else {
        std::fprintf(stderr,
                     "QemuBridge: clipboard registered (host↔guest bridge ready)\n");
    }
}

// ════════════════════════════════════════════════════════════════════════
QemuBridge::QemuBridge() : impl_(new Impl) { impl_->owner = this; }
QemuBridge::~QemuBridge() { stop(); delete impl_; }
QemuBridge::Impl* QemuBridge::impl() { return impl_; }

// Trampoline for g_thread_new.
static gpointer bridge_thread_trampoline(gpointer p) {
    static_cast<QemuBridge*>(p)->runGlibThread();
    return nullptr;
}

bool QemuBridge::start(const Config& cfg, std::string* err) {
    cfg_ = cfg;
    std::string qmpPath = cfg.runtimeDir + "/qmp.sock";
    ::unlink(qmpPath.c_str());

    // ── 1. Launch the QEMU launcher script (it exec()s qemu → child PID is
    //       qemu). DBUS_DISPLAY + QMP_SOCK switch the run script to our path.
    std::vector<std::string> envStrings = cfg.env;
    envStrings.push_back("DBUS_DISPLAY=1");
    envStrings.push_back("QMP_SOCK=" + qmpPath);
    std::vector<char*> envp;
    for (char** e = environ; *e; ++e) envp.push_back(*e);
    for (auto& s : envStrings) envp.push_back(const_cast<char*>(s.c_str()));
    envp.push_back(nullptr);

    const char* argv[] = {"bash", cfg.launcher.c_str(), nullptr};
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, "bash", nullptr, nullptr,
                          const_cast<char* const*>(argv), envp.data());
    if (rc != 0) {
        if (err) *err = std::string("posix_spawnp bash failed: ") + strerror(rc);
        return false;
    }
    qemuPid_ = pid;

    // ── 2. Connect to the QMP socket (QEMU creates it asynchronously).
    int qmp = -1;
    for (int i = 0; i < 200 && qmp < 0; ++i) {
        qmp = connectUnix(qmpPath);
        if (qmp < 0) usleep(50 * 1000);
    }
    if (qmp < 0) {
        if (err) *err = "could not connect to QMP socket " + qmpPath;
        stop();
        return false;
    }

    std::string buf;
    std::string greeting;
    qmpReadLine(qmp, buf, greeting);  // {"QMP": ...}
    if (!qmpSend(qmp, "{\"execute\":\"qmp_capabilities\"}") ||
        !qmpWaitReturn(qmp, buf)) {
        if (err) *err = "qmp_capabilities failed";
        close(qmp); stop(); return false;
    }

    // ── 3. socketpair → give one end to QEMU (add_client), keep the other as
    //       our p2p bus. QEMU authenticates as SERVER on its end.
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        if (err) *err = std::string("socketpair: ") + strerror(errno);
        close(qmp); stop(); return false;
    }
    if (!qmpSendWithFd(qmp, "{\"execute\":\"getfd\",\"arguments\":{\"fdname\":\"pomfd\"}}",
                       sv[1]) ||
        !qmpWaitReturn(qmp, buf)) {
        if (err) *err = "QMP getfd failed";
        close(sv[0]); close(sv[1]); close(qmp); stop(); return false;
    }
    close(sv[1]);  // QEMU dup'd it
    if (!qmpSend(qmp, "{\"execute\":\"add_client\",\"arguments\":"
                      "{\"protocol\":\"@dbus-display\",\"fdname\":\"pomfd\"}}") ||
        !qmpWaitReturn(qmp, buf)) {
        if (err) *err = "QMP add_client @dbus-display failed";
        close(sv[0]); close(qmp); stop(); return false;
    }
    // Keep the QMP channel for machine control (reset, pause, CD…). Bound the
    // reads so a wedged QEMU can't hang the UI thread. 10 s plutôt que 3 :
    // toutes ces commandes sont rapides sauf un changement de médium sur disque
    // lent, et un timeout coûte désormais le canal de contrôle entier (voir
    // qmpCommand) — autant ne l'atteindre que pour un QEMU réellement bloqué.
    struct timeval tv { 10, 0 };
    setsockopt(qmp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    qmpFd_ = qmp;
    qmpBuf_ = buf;  // carry over any bytes already read past the add_client reply

    impl_->mainFd = sv[0];
    running_.store(true, std::memory_order_relaxed);

    // ── 4. Everything D-Bus runs on a private GLib thread.
    thread_ = g_thread_new("qemu-dbus", bridge_thread_trampoline, this);
    return true;
}

void QemuBridge::runGlibThread() {
    Impl* d = impl_;
    d->ctx = g_main_context_new();
    g_main_context_push_thread_default(d->ctx);
    d->loop = g_main_loop_new(d->ctx, FALSE);

    GError* err = nullptr;
    GSocket* sock = g_socket_new_from_fd(d->mainFd, &err);
    if (!sock) {
        std::fprintf(stderr, "main g_socket_new_from_fd: %s\n",
                     err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    GSocketConnection* sc = g_socket_connection_factory_create_connection(sock);
    g_object_unref(sock);
    d->mainConn = g_dbus_connection_new_sync(
        G_IO_STREAM(sc), nullptr,
        (GDBusConnectionFlags)(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                               G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING),
        nullptr, nullptr, &err);
    g_object_unref(sc);
    if (!d->mainConn) {
        std::fprintf(stderr, "main dbus connection: %s\n",
                     err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    g_dbus_connection_start_message_processing(d->mainConn);

    const char* CONSOLE = "/org/qemu/Display1/Console_0";
    d->console = qemu_dbus_display1_console_proxy_new_sync(
        d->mainConn, G_DBUS_PROXY_FLAGS_NONE, nullptr, CONSOLE, nullptr, &err);
    if (!d->console) {
        std::fprintf(stderr, "Console_0 proxy: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    d->keyboard = qemu_dbus_display1_keyboard_proxy_new_sync(
        d->mainConn, G_DBUS_PROXY_FLAGS_NONE, nullptr, CONSOLE, nullptr, nullptr);
    d->mouse = qemu_dbus_display1_mouse_proxy_new_sync(
        d->mainConn, G_DBUS_PROXY_FLAGS_NONE, nullptr, CONSOLE, nullptr, nullptr);
    if (d->mouse) {
        d->mouseIsAbs = qemu_dbus_display1_mouse_get_is_absolute(d->mouse);
        mouseAbs_.store(d->mouseIsAbs, std::memory_order_relaxed);
    }

    // ── Clipboard peer (both sides implement it on the main connection) ──
    d->clipIface = qemu_dbus_display1_clipboard_skeleton_new();
    g_signal_connect(d->clipIface, "handle-register", G_CALLBACK(on_clip_register), this);
    g_signal_connect(d->clipIface, "handle-unregister", G_CALLBACK(on_clip_unregister), this);
    g_signal_connect(d->clipIface, "handle-grab", G_CALLBACK(on_clip_grab), this);
    g_signal_connect(d->clipIface, "handle-request", G_CALLBACK(on_clip_request), this);
    g_signal_connect(d->clipIface, "handle-release", G_CALLBACK(on_clip_release), this);
    { const gchar* none[] = {nullptr}; g_object_set(d->clipIface, "interfaces", none, nullptr); }
    if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(d->clipIface),
                                          d->mainConn, "/org/qemu/Display1/Clipboard", &err)) {
        std::fprintf(stderr, "clipboard export: %s\n", err ? err->message : "?");
        g_clear_error(&err);
    }
    d->clipProxy = qemu_dbus_display1_clipboard_proxy_new_sync(
        d->mainConn,
        (GDBusProxyFlags)(G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START |
                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES),
        nullptr, "/org/qemu/Display1/Clipboard", nullptr, &err);
    if (!d->clipProxy) {
        std::fprintf(stderr, "clipboard proxy: %s\n", err ? err->message : "?");
        g_clear_error(&err);
    } else {
        qemu_dbus_display1_clipboard_call_register(
            d->clipProxy, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, on_clip_registered, this);
    }

    // Register our display listener (second socketpair; QEMU = auth server).
    int lp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, lp) != 0) {
        std::fprintf(stderr, "listener socketpair: %s\n", strerror(errno));
        return;
    }
    d->listenerLocalFd = lp[0];
    d->regFdList = g_unix_fd_list_new();
    int handle = g_unix_fd_list_append(d->regFdList, lp[1], nullptr);
    close(lp[1]);  // fd list dup'd it
    // The client end (lp[0]) is built in on_listener_registered, once QEMU is
    // serving the socket.
    qemu_dbus_display1_console_call_register_listener(
        d->console, g_variant_new_handle(handle), G_DBUS_CALL_FLAGS_NONE, -1,
        d->regFdList, nullptr, on_listener_registered, this);

    g_main_loop_run(d->loop);

    // Teardown.
    g_main_context_pop_thread_default(d->ctx);
}

void QemuBridge::stop() {
    running_.store(false, std::memory_order_relaxed);
    if (qmpFd_ >= 0) { close(qmpFd_); qmpFd_ = -1; }
    if (impl_ && impl_->loop) g_main_loop_quit(impl_->loop);
    if (thread_) { g_thread_join(thread_); thread_ = nullptr; }
    if (impl_ && impl_->mapAddr) {   // D-Bus thread gone → safe to unmap
        munmap(impl_->mapAddr, impl_->mapLen);
        impl_->mapAddr = nullptr; impl_->mapData = nullptr; impl_->mapLen = 0;
    }
    if (qemuPid_ > 0 && !qemuReaped_) {
        kill(qemuPid_, SIGTERM);
        bool reaped = false;
        for (int i = 0; i < 40 && !reaped; ++i) {
            if (waitpid(qemuPid_, nullptr, WNOHANG) == qemuPid_) reaped = true;
            else usleep(50 * 1000);
        }
        // Only escalate if it is still there. Signalling an already-reaped pid
        // is not harmless: the number is free for reuse the instant waitpid()
        // returns, and relaunch() spawns a new process immediately after —
        // so the stray SIGKILL could land on an unrelated process.
        if (!reaped) {
            kill(qemuPid_, SIGKILL);
            waitpid(qemuPid_, nullptr, 0);
        }
    }
    qemuPid_ = -1;
    qemuReaped_ = false;
}

bool QemuBridge::checkAlive() {
    if (qemuPid_ <= 0 || qemuReaped_) return false;
    if (waitpid(qemuPid_, nullptr, WNOHANG) == qemuPid_) {
        qemuReaped_ = true;
        running_.store(false, std::memory_order_relaxed);
        std::fprintf(stderr, "QemuBridge: QEMU (pid %ld) a quitté\n", qemuPid_);
        return false;
    }
    return true;
}

// ── Framebuffer ingest (D-Bus thread) → shared buffer ────────────────────
void QemuBridge::ingestScanout(uint32_t w, uint32_t h, uint32_t stride,
                               uint32_t pixmanFormat, const uint8_t* data,
                               size_t len) {
    static bool once = false;
    if (!once) {
        std::fprintf(stderr, "QemuBridge: first Scanout %ux%u stride=%u fmt=0x%x len=%zu\n",
                     w, h, stride, pixmanFormat, len);
        once = true;
    }
    if (w == 0 || h == 0 || stride < w * 4) return;  // P1: 32bpp only
    std::lock_guard<std::mutex> lk(fbMtx_);
    if ((int)w != fbW_ || (int)h != fbH_) fbResized_ = true;
    fbW_ = (int)w;
    fbH_ = (int)h;
    fb_.assign((size_t)w * h, 0);
    for (uint32_t row = 0; row < h; ++row) {
        size_t off = (size_t)row * stride;
        if (off + w * 4 > len) break;
        std::memcpy(&fb_[(size_t)row * w], data + off, (size_t)w * 4);
    }
    markRows(0, fbH_);
}

// Widen the dirty row span. fbMtx_ is held by the caller.
void QemuBridge::markRows(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > fbH_) y1 = fbH_;
    if (y0 >= y1) return;
    if (!fbDirty_) { fbY0_ = y0; fbY1_ = y1; fbDirty_ = true; return; }
    if (y0 < fbY0_) fbY0_ = y0;
    if (y1 > fbY1_) fbY1_ = y1;
}

void QemuBridge::ingestUpdate(int x, int y, int w, int h, uint32_t stride,
                              uint32_t /*fmt*/, const uint8_t* data,
                              size_t len) {
    std::lock_guard<std::mutex> lk(fbMtx_);
    if (fbW_ == 0 || fbH_ == 0) return;
    if (w <= 0 || h <= 0 || stride < (uint32_t)w * 4) return;

    // Clamp the span once, then one memcpy per row. The previous version did a
    // 4-byte memcpy per pixel with two branches: a full-screen update meant
    // 786 000 calls instead of 768 copies of 4 KiB.
    int sx = x < 0 ? -x : 0;                       // first source column kept
    int dx0 = x + sx;
    int cols = w - sx;
    if (dx0 + cols > fbW_) cols = fbW_ - dx0;
    if (cols <= 0) return;

    for (int row = 0; row < h; ++row) {
        int dy = y + row;
        if (dy < 0 || dy >= fbH_) continue;
        size_t off = (size_t)row * stride + (size_t)sx * 4;
        if (off + (size_t)cols * 4 > len) break;
        std::memcpy(&fb_[(size_t)dy * fbW_ + dx0], data + off, (size_t)cols * 4);
    }
    markRows(y, y + h);
}

// ── Unix.Map fast path (D-Bus thread) ────────────────────────────────────
void QemuBridge::mapScanout(int fd, uint32_t offset, uint32_t w, uint32_t h,
                            uint32_t stride, uint32_t pixmanFormat) {
    static bool once = false;
    if (!once) {
        std::fprintf(stderr,
                     "QemuBridge: ScanoutMap (shared-mem fast path) %ux%u "
                     "stride=%u off=%u fmt=0x%x\n",
                     w, h, stride, offset, pixmanFormat);
        once = true;
    }
    if (impl_->mapAddr) {
        munmap(impl_->mapAddr, impl_->mapLen);
        impl_->mapAddr = nullptr; impl_->mapData = nullptr; impl_->mapLen = 0;
    }
    if (w == 0 || h == 0 || stride < w * 4) { close(fd); return; }

    long page = sysconf(_SC_PAGESIZE);
    off_t aoff = (off_t)offset & ~(off_t)(page - 1);
    size_t extra = (size_t)(offset - aoff);
    size_t maplen = (size_t)h * stride + extra;
    void* base = mmap(nullptr, maplen, PROT_READ, MAP_SHARED, fd, aoff);
    close(fd);
    if (base == MAP_FAILED) {
        std::fprintf(stderr, "ScanoutMap mmap failed: %s\n", strerror(errno));
        return;
    }
    impl_->mapAddr = base;
    impl_->mapLen = maplen;
    impl_->mapData = static_cast<const uint8_t*>(base) + extra;
    impl_->mapW = w; impl_->mapH = h; impl_->mapStride = stride;

    // Full surface → framebuffer.
    std::lock_guard<std::mutex> lk(fbMtx_);
    if ((int)w != fbW_ || (int)h != fbH_) fbResized_ = true;
    fbW_ = (int)w; fbH_ = (int)h;
    fb_.assign((size_t)w * h, 0);
    for (uint32_t row = 0; row < h; ++row)
        std::memcpy(&fb_[(size_t)row * w],
                    impl_->mapData + (size_t)row * stride, (size_t)w * 4);
    markRows(0, fbH_);
}

void QemuBridge::mapUpdate(int x, int y, int w, int h) {
    std::lock_guard<std::mutex> lk(fbMtx_);
    if (!impl_->mapData || fbW_ == 0 || w <= 0 || h <= 0) return;

    // Bound by the MAPPING as well as by the framebuffer. An inline Scanout
    // arriving after a ScanoutMap resizes fb_ but leaves mapW/mapH/mapStride
    // describing the old, smaller mmap — reading at fb_ dimensions then walked
    // off the end of the mapping.
    int maxW = fbW_ < (int)impl_->mapW ? fbW_ : (int)impl_->mapW;
    int maxH = fbH_ < (int)impl_->mapH ? fbH_ : (int)impl_->mapH;

    int dx0 = x < 0 ? 0 : x;
    int cols = x + w - dx0;
    if (dx0 + cols > maxW) cols = maxW - dx0;
    if (cols <= 0) return;

    for (int row = 0; row < h; ++row) {
        int dy = y + row;
        if (dy < 0 || dy >= maxH) continue;
        const uint8_t* src = impl_->mapData + (size_t)dy * impl_->mapStride;
        std::memcpy(&fb_[(size_t)dy * fbW_ + dx0], src + (size_t)dx0 * 4,
                    (size_t)cols * 4);
    }
    markRows(y, y + h);
}

bool QemuBridge::latchFrame(std::vector<uint32_t>& out, int& w, int& h,
                            int& y0, int& y1, bool& resized) {
    std::lock_guard<std::mutex> lk(fbMtx_);
    if (!fbDirty_ || fbW_ == 0) return false;

    w = fbW_;
    h = fbH_;
    resized = fbResized_ || out.size() != fb_.size();
    if (resized) {
        out = fb_;                       // geometry changed: full copy
        y0 = 0; y1 = fbH_;
    } else {
        y0 = fbY0_; y1 = fbY1_;
        if (y1 > y0) {
            std::memcpy(&out[(size_t)y0 * fbW_], &fb_[(size_t)y0 * fbW_],
                        (size_t)(y1 - y0) * fbW_ * 4);
        }
    }
    fbDirty_ = false;
    fbResized_ = false;
    return true;
}

bool QemuBridge::latchFrame(std::vector<uint32_t>& out, int& w, int& h) {
    int y0, y1; bool resized;
    return latchFrame(out, w, h, y0, y1, resized);
}

// ── Input: marshal onto the D-Bus thread via g_main_context_invoke ───────
namespace {
struct InputOp {
    QemuBridge::Impl* d;
    enum { KeyPress, KeyRelease, MouseAbs, MouseRel, MousePress, MouseRelease } type;
    guint a = 0, b = 0;
};
gboolean run_input(gpointer p) {
    auto* o = static_cast<InputOp*>(p);
    QemuBridge::Impl* d = o->d;
    switch (o->type) {
        case InputOp::KeyPress:
            if (d->keyboard)
                qemu_dbus_display1_keyboard_call_press(d->keyboard, o->a,
                    G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
            break;
        case InputOp::KeyRelease:
            if (d->keyboard)
                qemu_dbus_display1_keyboard_call_release(d->keyboard, o->a,
                    G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
            break;
        case InputOp::MouseAbs:
            if (d->mouse && d->mouseIsAbs)
                qemu_dbus_display1_mouse_call_set_abs_position(d->mouse, o->a,
                    o->b, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
            break;
        case InputOp::MouseRel:
            if (d->mouse && !d->mouseIsAbs)
                qemu_dbus_display1_mouse_call_rel_motion(d->mouse, (gint)o->a,
                    (gint)o->b, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
            break;
        case InputOp::MousePress:
            if (d->mouse)
                qemu_dbus_display1_mouse_call_press(d->mouse, o->a,
                    G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
            break;
        case InputOp::MouseRelease:
            if (d->mouse)
                qemu_dbus_display1_mouse_call_release(d->mouse, o->a,
                    G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
            break;
    }
    return G_SOURCE_REMOVE;
}
void post(QemuBridge::Impl* d, InputOp op) {
    if (!d || !d->ctx) return;
    auto* o = new InputOp(op);
    o->d = d;
    g_main_context_invoke_full(d->ctx, G_PRIORITY_DEFAULT, run_input, o,
                               [](gpointer x) { delete static_cast<InputOp*>(x); });
}
}  // namespace

void QemuBridge::keyPress(uint32_t k)   { post(impl_, {impl_, InputOp::KeyPress, k}); }
void QemuBridge::keyRelease(uint32_t k) { post(impl_, {impl_, InputOp::KeyRelease, k}); }
void QemuBridge::mouseButton(int button, bool down) {
    post(impl_, {impl_, down ? InputOp::MousePress : InputOp::MouseRelease,
                 (guint)button});
}
void QemuBridge::mouseWheel(int dir) {
    post(impl_, {impl_, InputOp::MousePress, (guint)dir});
    post(impl_, {impl_, InputOp::MouseRelease, (guint)dir});
}

// ── Machine control over QMP ─────────────────────────────────────────────
namespace {
std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\' || c == '"') o += '\\';
        o += c;
    }
    return o;
}
}  // namespace

bool QemuBridge::qmpCommand(const std::string& json) {
    if (qmpFd_ < 0) return false;
    std::lock_guard<std::mutex> lk(qmpMtx_);
    if (json.size() < 2 || json.back() != '}') return false;

    // Tag the command so its reply can be told apart from QEMU's asynchronous
    // events (see qmpWaitId).
    char idbuf[32];
    std::snprintf(idbuf, sizeof idbuf, "pom-%u", ++qmpSeq_);
    std::string tagged = json.substr(0, json.size() - 1) +
                         ",\"id\":\"" + idbuf + "\"}";

    if (!qmpSend(qmpFd_, tagged)) return false;

    bool fatal = false;
    // QEMU echoes the id back as {"return": {}, "id": "pom-1"}; be tolerant of
    // spacing by matching on the id value alone.
    std::string needle = std::string("\"") + idbuf + "\"";
    bool ok = qmpWaitId(qmpFd_, qmpBuf_, needle, &fatal);
    if (fatal) {
        // A read timeout leaves half a line in qmpBuf_; every later command
        // would then parse garbage. Drop the channel instead of limping on.
        std::fprintf(stderr, "QemuBridge: canal QMP perdu — contrôle désactivé\n");
        close(qmpFd_);
        qmpFd_ = -1;
        qmpBuf_.clear();
    }
    return ok;
}
bool QemuBridge::reset() { return qmpCommand("{\"execute\":\"system_reset\"}"); }
bool QemuBridge::setPaused(bool p) {
    return qmpCommand(p ? "{\"execute\":\"stop\"}" : "{\"execute\":\"cont\"}");
}
bool QemuBridge::ejectCd(const std::string& id) {
    return qmpCommand("{\"execute\":\"eject\",\"arguments\":{\"device\":\"" +
                      id + "\",\"force\":true}}");
}
bool QemuBridge::changeCd(const std::string& path, const std::string& id) {
    return qmpCommand(
        "{\"execute\":\"blockdev-change-medium\",\"arguments\":{\"device\":\"" +
        id + "\",\"filename\":\"" + jsonEsc(path) + "\",\"format\":\"raw\"}}");
}

// ── Clipboard bridge ─────────────────────────────────────────────────────
void QemuBridge::clipResetSerial() { std::lock_guard<std::mutex> lk(clipMtx_); clipSerial_ = 0; }
void QemuBridge::clipStoreGuest(const std::string& t) {
    std::lock_guard<std::mutex> lk(clipMtx_);
    guestClip_ = t; guestClipReady_ = true;
}
std::string QemuBridge::clipLocalText() {
    std::lock_guard<std::mutex> lk(clipMtx_);
    return localClip_;
}
bool QemuBridge::takeGuestClipboard(std::string& out) {
    std::lock_guard<std::mutex> lk(clipMtx_);
    if (!guestClipReady_) return false;
    out = guestClip_; guestClipReady_ = false;
    return true;
}
void QemuBridge::publishLocalClipboard(const std::string& text) {
    if (text.empty()) return;
    {
        std::lock_guard<std::mutex> lk(clipMtx_);
        if (text == lastLocalSent_) return;
        lastLocalSent_ = text;
        localClip_ = text;
    }
    // Announce the new host clipboard to the guest (Grab) on the D-Bus thread.
    if (impl_ && impl_->ctx)
        g_main_context_invoke_full(
            impl_->ctx, G_PRIORITY_DEFAULT,
            [](gpointer p) -> gboolean {
                static_cast<QemuBridge*>(p)->doClipGrab();
                return G_SOURCE_REMOVE;
            },
            this, nullptr);
}
bool QemuBridge::requestClipboard(std::string& out) {
    if (!impl_ || !impl_->clipProxy) return false;
    const gchar* want[] = {"text/plain;charset=utf-8", "text/plain", nullptr};
    gchar* rmime = nullptr; GVariant* data = nullptr; GError* err = nullptr;
    bool ok = qemu_dbus_display1_clipboard_call_request_sync(
        impl_->clipProxy, 0, want, G_DBUS_CALL_FLAGS_NONE, 2000, &rmime, &data,
        nullptr, &err);
    if (!ok) { if (err) g_clear_error(&err); return false; }
    gsize n = g_variant_get_size(data);
    const char* b = static_cast<const char*>(g_variant_get_data(data));
    out.assign(b ? b : "", b ? n : 0);
    g_variant_unref(data);
    g_free(rmime);
    return true;
}
void QemuBridge::doClipGrab() {
    if (!impl_ || !impl_->clipProxy) return;
    const gchar* mimes[] = {"text/plain;charset=utf-8", "text/plain", nullptr};
    guint serial = ++clipSerial_;   // D-Bus thread only
    qemu_dbus_display1_clipboard_call_grab(impl_->clipProxy, 0, serial, mimes,
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, on_clip_grab_done, this);
    std::fprintf(stderr, "QemuBridge: clipboard host→guest grab (serial %u)\n", serial);
}
void QemuBridge::mouseAbs(int x, int y) {
    // QEMU rejects SetAbsPosition when x>=width or y>=height, so clamp to the
    // current guest surface.
    int w, h;
    { std::lock_guard<std::mutex> lk(fbMtx_); w = fbW_; h = fbH_; }
    if (w <= 0 || h <= 0) return;
    if (x < 0) x = 0; else if (x >= w) x = w - 1;
    if (y < 0) y = 0; else if (y >= h) y = h - 1;
    post(impl_, {impl_, InputOp::MouseAbs, (guint)x, (guint)y});
}
void QemuBridge::mouseRel(int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    post(impl_, {impl_, InputOp::MouseRel, (guint)dx, (guint)dy});
}
bool QemuBridge::mouseIsAbsolute() const {
    return mouseAbs_.load(std::memory_order_relaxed);
}
