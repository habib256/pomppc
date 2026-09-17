#!/usr/bin/env python3
"""gen_tramp.py — génère les trampolines PowerPC du plugin OpenGL POMPPC.

Deux familles, toutes deux sans toucher aux arguments (entiers r3–r10,
flottants f1–f6, pile) :

  * `_gldXxx` : points d'entrée exportés vers GLEngine ;
  * `_pomppc_proc_N` : mandataires des procédures de rastérisation installées
    par gldInitDispatch.

Chaque trampoline appelle `_pomppc_pre(id, args)` — trace, synchronisation
invité/hôte — qui RENVOIE l'adresse de la fonction réelle à appeler (la
fonction du rendu logiciel d'Apple ; pour une procédure, celle du contexte
passé en premier argument). Le trampoline restaure alors les registres et
saute (bctr) dans cette fonction : l'appelé voit exactement l'appel d'origine.

Les fonctions que le plugin réimplémente en C sont exclues des exports
générés (liste C_OVERRIDES) : leur symbole vient de pomppc_gld.c.

    python3 tools/gld/gen_tramp.py > guest/gldriver/gld_tramp.s
"""
import sys

# ordre = table de GLEngine (glep_gld_names), puis Initialize/Terminate
GLD = """GetVersion ChoosePixelFormat DestroyPixelFormat GetRendererInfo CreateShared
DestroyShared CreateContext DestroyContext ReclaimContext AttachDrawable GetInteger
SetInteger InitDispatch UpdateDispatch CreateTexture CreateTextureLevel ModifyTexture
ModifyTextureLevel GetTextureLevelInfo GetTextureLevel DeleteTextureLevel DeleteTexture
IsTextureResident ReclaimTexture Flush Finish GetString GetError AllocVertexBuffer
CompleteVertexBuffer FreeVertexBuffer CreatePipelineProgram ModifyPipelineProgram
RelatePipelineProgram GetPipelineProgramInfo DestroyPipelineProgram CreateVertexArray
ModifyVertexArray FlushVertexArray DestroyVertexArray ReclaimVertexArray CreateFence
DestroyFence TestObject FinishObject CreateQuery DestroyQuery GetQueryInfo CreateBuffer
DestroyBuffer FlushBuffer ReclaimBuffer PageoffBuffer GetMemoryPluginData
SetMemoryPluginData FinishMemoryPluginData TestMemoryPluginData DestroyMemoryPluginData
CreateFramebuffer ReclaimFramebuffer DestroyFramebuffer InitializeLibrary
TerminateLibrary""".split()

C_OVERRIDES = {"GetVersion", "ChoosePixelFormat", "DestroyPixelFormat", "GetRendererInfo", "CreateContext",
               "DestroyContext", "AttachDrawable", "InitDispatch", "UpdateDispatch",
               "Flush", "Finish", "GetString", "InitializeLibrary", "TerminateLibrary",
               "GetInteger", "CreateTexture", "DeleteTexture", "CreateTextureLevel",
               "ModifyTextureLevel", "DeleteTextureLevel", "ModifyTexture"}

NPROCS = 36          # emplacements de la table de procédures (0x00..0x8c)
FRAME = 144          # 24 liaison + 32 paramètres + 8×4 + 6×8 flottants, aligné 16


def tramp(label, hookid):
    s = []
    s.append("\t.globl %s" % label if label.startswith("_gld") else "\t.private_extern %s" % label)
    s.append("\t.align 2")
    s.append("%s:" % label)
    s.append("\tmflr r0")
    s.append("\tstw r0,8(r1)")
    s.append("\tstwu r1,-%d(r1)" % FRAME)
    for i, r in enumerate(range(3, 11)):
        s.append("\tstw r%d,%d(r1)" % (r, 56 + 4 * i))
    for i, f in enumerate(range(1, 7)):
        s.append("\tstfd f%d,%d(r1)" % (f, 88 + 8 * i))
    s.append("\tli r3,%d" % hookid)
    s.append("\taddi r4,r1,56")
    s.append("\tbl _pomppc_pre")
    s.append("\tmr r12,r3")
    for i, r in enumerate(range(3, 11)):
        s.append("\tlwz r%d,%d(r1)" % (r, 56 + 4 * i))
    for i, f in enumerate(range(1, 7)):
        s.append("\tlfd f%d,%d(r1)" % (f, 88 + 8 * i))
    s.append("\taddi r1,r1,%d" % FRAME)
    s.append("\tlwz r0,8(r1)")
    s.append("\tmtlr r0")
    s.append("\tmtctr r12")
    s.append("\tbctr")
    s.append("")
    return "\n".join(s)


def main():
    out = ["; gld_tramp.s — GÉNÉRÉ par tools/gld/gen_tramp.py, ne pas éditer.",
           "; Trampolines du plugin OpenGL POMPPC (voir pomppc_gld.c).", "",
           "\t.text", ""]
    for i, n in enumerate(GLD):
        if n in C_OVERRIDES:
            continue
        out.append(tramp("_gld" + n, i))
    for k in range(NPROCS):
        out.append(tramp("_pomppc_proc_%d" % k, 1000 + k))
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--names":
        print("\n".join(GLD))
    else:
        main()
