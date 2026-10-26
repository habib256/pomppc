/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/* qgpu-light.h — éclairage par sommet d'OpenGL 1.x (formule de la spécification),
 * commun aux backends : le soft l'emploie pour tout dessin éclairé ; le backend gl
 * pour les dessins où l'éclairage fixe de l'HÔTE est faux (26/10/2026 : le GL
 * d'Apple sur Metal, dès qu'une lumière spot est allumée, retranche aux points hors
 * de son cône l'éclairage des AUTRES lumières — UT2004, cargaison et présentoirs
 * noirs, docs/re/ut2004-arme-noire.md). Déplacé tel quel de qgpu-soft.c : mêmes
 * opérations dans le même ordre, le rendu soft ne change pas d'un bit.
 */
#ifndef QGPU_LIGHT_H
#define QGPU_LIGHT_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "qgpu-core.h"

/* Ce que l'éclairage lit de l'état, pris une fois par dessin. */
typedef struct QgpuLightCfg {
    const QgpuGeom *gm;
    float    inv3[9];          /* inverse 3×3 de la modèle-vue, rangée par LIGNES */
    float    rescale;          /* facteur de GL_RESCALE_NORMAL */
    bool     normalize, rescale_normal;
    bool     two_side, sep_spec, local_viewer, color_material, cm_front, cm_back;
    uint32_t cm_mode;
} QgpuLightCfg;

static inline float ql_clamp01(float v)
{
    return !(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v;      /* un NaN donne 0 */
}

static inline float ql_dot3(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline void ql_norm3(float *v)
{
    float l = sqrtf(ql_dot3(v, v));
    if (l > 0.0f) {
        v[0] /= l; v[1] /= l; v[2] /= l;
    }
}

/* m est en ORDRE COLONNE : m[4c + r] est la ligne r, colonne c. */
static inline void ql_mat_vec4(const float *m, const float *v, float *o)
{
    int i;
    for (i = 0; i < 4; i++) {
        o[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3];
    }
}

/* Inverse du bloc 3×3 supérieur gauche, rangé par LIGNES (o[3r + c]). */
static inline bool ql_mat3_inverse(const float *m, float *o)
{
    float a00 = m[0], a01 = m[4], a02 = m[8];
    float a10 = m[1], a11 = m[5], a12 = m[9];
    float a20 = m[2], a21 = m[6], a22 = m[10];
    float c00 =  (a11 * a22 - a12 * a21);
    float c01 = -(a10 * a22 - a12 * a20);
    float c02 =  (a10 * a21 - a11 * a20);
    float det = a00 * c00 + a01 * c01 + a02 * c02;

    if (det == 0.0f || det != det) {
        return false;
    }
    o[0] = c00 / det;
    o[1] = -(a01 * a22 - a02 * a21) / det;
    o[2] =  (a01 * a12 - a02 * a11) / det;
    o[3] = c01 / det;
    o[4] =  (a00 * a22 - a02 * a20) / det;
    o[5] = -(a00 * a12 - a02 * a10) / det;
    o[6] = c02 / det;
    o[7] = -(a00 * a21 - a01 * a20) / det;
    o[8] =  (a00 * a11 - a01 * a10) / det;
    return true;
}

/* GL : n' = n · M⁻¹ (vecteur ligne), soit (M⁻¹)ᵀ · n en colonne. */
static inline void ql_xform_normal(const float *inv3, const float *n, float *o)
{
    o[0] = inv3[0] * n[0] + inv3[3] * n[1] + inv3[6] * n[2];
    o[1] = inv3[1] * n[0] + inv3[4] * n[1] + inv3[7] * n[2];
    o[2] = inv3[2] * n[0] + inv3[5] * n[1] + inv3[8] * n[2];
}

/* Les drapeaux de l'éclairage, l'inverse de la modèle-vue et le facteur de
   GL_RESCALE_NORMAL, pour un dessin. */
static inline void ql_cfg(QgpuLightCfg *L, const QgpuState *st, const QgpuGeom *gm)
{
    float l;
    memset(L, 0, sizeof(*L));
    L->gm = gm;
    L->normalize = st->v[QGPU_SK_NORMALIZE] != 0;
    L->rescale_normal = st->v[QGPU_SK_RESCALE_NORMAL] != 0;
    L->two_side = st->v[QGPU_SK_TWO_SIDE] != 0;
    L->sep_spec = st->v[QGPU_SK_COLOR_CONTROL] == 0x81FA;
    L->local_viewer = st->v[QGPU_SK_LOCAL_VIEWER] != 0;
    L->color_material = st->v[QGPU_SK_COLOR_MATERIAL] != 0;
    L->cm_front = st->v[QGPU_SK_COLOR_MAT_FACE] != 0x0405;      /* pas GL_BACK seul */
    L->cm_back = st->v[QGPU_SK_COLOR_MAT_FACE] != 0x0404;       /* pas GL_FRONT seul */
    L->cm_mode = st->v[QGPU_SK_COLOR_MAT_MODE];
    if (!ql_mat3_inverse(gm->mtx[QGPU_MTX_MODELVIEW], L->inv3)) {
        /* Modèle-vue singulière : GL laisse le résultat indéfini ; l'identité
           vaut mieux que des NaN dans le rasteriseur. */
        memset(L->inv3, 0, sizeof(L->inv3));
        L->inv3[0] = L->inv3[4] = L->inv3[8] = 1.0f;
    }
    /* GL_RESCALE_NORMAL : 1/‖3e ligne de l'inverse de la modèle-vue‖. */
    l = sqrtf(L->inv3[6] * L->inv3[6] + L->inv3[7] * L->inv3[7] + L->inv3[8] * L->inv3[8]);
    L->rescale = (l > 0.0f) ? 1.0f / l : 1.0f;
}

/* La normale d'objet passée en œil, avec GL_NORMALIZE ou GL_RESCALE_NORMAL. */
static inline void ql_normal(const QgpuLightCfg *L, const float *nrm, float *neye)
{
    int k;
    ql_xform_normal(L->inv3, nrm, neye);
    if (L->normalize) {
        ql_norm3(neye);
    } else if (L->rescale_normal) {
        for (k = 0; k < 3; k++) {
            neye[k] *= L->rescale;
        }
    }
}

/* Éclairage d'OpenGL 1.x, formule de la spécification, pour UNE face.
   `nrm` est déjà retournée quand on éclaire la face arrière. */
static inline void ql_face(const QgpuLightCfg *L, const float *eye, const float *nrm,
                           const float *vcol, int face, float *col, float *sec)
{
    const QgpuGeom *gm = L->gm;
    const QgpuMaterial *m = &gm->mat[face];
    float amb[4], dif[4], spc[4], emi[4], shin = m->shininess;
    float acc[3], sacc[3];
    float pe[3], vpe[3];
    bool cm = L->color_material && (face == 0 ? L->cm_front : L->cm_back);
    int i, k;

    memcpy(amb, m->ambient, sizeof(amb));
    memcpy(dif, m->diffuse, sizeof(dif));
    memcpy(spc, m->specular, sizeof(spc));
    memcpy(emi, m->emission, sizeof(emi));
    if (cm) {
        switch (L->cm_mode) {
        case 0x1600: memcpy(emi, vcol, sizeof(emi)); break;      /* GL_EMISSION */
        case 0x1200: memcpy(amb, vcol, sizeof(amb)); break;      /* GL_AMBIENT */
        case 0x1201: memcpy(dif, vcol, sizeof(dif)); break;      /* GL_DIFFUSE */
        case 0x1202: memcpy(spc, vcol, sizeof(spc)); break;      /* GL_SPECULAR */
        default:                                                 /* AMBIENT_AND_DIFFUSE */
            memcpy(amb, vcol, sizeof(amb));
            memcpy(dif, vcol, sizeof(dif));
            break;
        }
    }
    for (k = 0; k < 3; k++) {
        pe[k] = (eye[3] != 0.0f) ? eye[k] / eye[3] : eye[k];
        acc[k] = emi[k] + amb[k] * gm->lm_ambient[k];
        sacc[k] = 0.0f;
    }
    if (L->local_viewer) {
        vpe[0] = -pe[0]; vpe[1] = -pe[1]; vpe[2] = -pe[2];
        ql_norm3(vpe);
    } else {
        vpe[0] = 0.0f; vpe[1] = 0.0f; vpe[2] = 1.0f;
    }
    for (i = 0; i < QGPU_MAX_LIGHTS; i++) {
        const QgpuLight *l = &gm->light[i];
        float vp[3], att = 1.0f, spot = 1.0f, ndotvp, f;
        if (!l->enabled) {
            continue;
        }
        if (l->position[3] != 0.0f) {
            float d, den;
            for (k = 0; k < 3; k++) {
                vp[k] = l->position[k] / l->position[3] - pe[k];
            }
            d = sqrtf(ql_dot3(vp, vp));
            if (d > 0.0f) {
                vp[0] /= d; vp[1] /= d; vp[2] /= d;
            }
            den = l->att[0] + l->att[1] * d + l->att[2] * d * d;
            att = (den > 0.0f) ? 1.0f / den : 1.0f;
            if (l->spot_cutoff != 180.0f) {
                float sd[3], cosang;
                memcpy(sd, l->spot_dir, sizeof(sd));
                ql_norm3(sd);
                /* angle entre la direction lumière → sommet et l'axe du spot */
                cosang = -ql_dot3(vp, sd);
                if (cosang <= 0.0f ||
                    cosang < cosf(l->spot_cutoff * 3.14159265358979f / 180.0f)) {
                    spot = 0.0f;
                } else {
                    spot = powf(cosang, l->spot_exp);
                }
            }
        } else {
            memcpy(vp, l->position, 3 * sizeof(float));
            ql_norm3(vp);                     /* lumière directionnelle */
        }
        f = att * spot;
        for (k = 0; k < 3; k++) {
            acc[k] += f * amb[k] * l->ambient[k];
        }
        ndotvp = ql_dot3(nrm, vp);
        if (ndotvp > 0.0f) {
            float h[3], ndoth, sf;
            for (k = 0; k < 3; k++) {
                acc[k] += f * ndotvp * dif[k] * l->diffuse[k];
                h[k] = vp[k] + vpe[k];
            }
            ql_norm3(h);
            ndoth = ql_dot3(nrm, h);
            sf = (ndoth > 0.0f) ? powf(ndoth, shin) : 0.0f;
            for (k = 0; k < 3; k++) {
                float v = f * sf * spc[k] * l->specular[k];
                if (L->sep_spec) {
                    sacc[k] += v;
                } else {
                    acc[k] += v;
                }
            }
        }
    }
    for (k = 0; k < 3; k++) {
        col[k] = ql_clamp01(acc[k]);
        sec[k] = ql_clamp01(sacc[k]);
    }
    col[3] = ql_clamp01(dif[3]);              /* GL : l'alpha vient de la diffuse */
}

#endif /* QGPU_LIGHT_H */
