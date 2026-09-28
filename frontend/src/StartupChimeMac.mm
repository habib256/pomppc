/* GPL3 - Copyleft VERHILLE Arnaud */
#import <AppKit/AppKit.h>
#include "StartupChime.h"
#include <algorithm>

struct ChimePlayer::Impl { NSSound* sound = nil; };
ChimePlayer::ChimePlayer() : impl_(new Impl) {}
ChimePlayer::~ChimePlayer() { stop(); }
void ChimePlayer::stop() {
    [impl_->sound stop];
    [impl_->sound release];
    impl_->sound = nil;
}
void ChimePlayer::volume(float value) {
    [impl_->sound setVolume:std::clamp(value, 0.0f, 1.0f)];
}
bool ChimePlayer::play(const std::string& file, float level, std::string& error) {
    stop();
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:file.c_str()];
        NSString* ext = [[path pathExtension] lowercaseString];
        if (!([ext isEqualToString:@"wav"] || [ext isEqualToString:@"aiff"] ||
              [ext isEqualToString:@"aif"])) {
            error = "format attendu : WAV ou AIFF";
            return false;
        }
        impl_->sound = [[NSSound alloc] initWithContentsOfFile:path byReference:NO];
        volume(level);
        if (!impl_->sound || ![impl_->sound play]) {
            error = "fichier absent, illisible ou sortie audio indisponible : " + file;
            stop();
            return false;
        }
    }
    return true;
}
