/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_math3d.c
// 3D math helpers: vec2/vec3/vec4 and mat4 operations for Kirton.

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool read_vec(const Value* v, int n, double* out){
    if (!v || v->kind != VAL_LIST) return false;
    if ((int)v->as.list->len != n) return false;
    for (int i=0; i<n; ++i){
        Value* it = &v->as.list->data[i];
        if (it->kind != VAL_NUM) return false;
        out[i] = it->as.num;
    }
    return true;
}

static Value make_vec(const double* v, int n){
    Value L = v_list_new();
    for (int i=0; i<n; ++i) v_list_push(&L, v_num(v[i]));
    return L;
}

static bool read_mat4(const Value* v, double* out16){
    if (!v || v->kind != VAL_LIST) return false;
    if (v->as.list->len != 16) return false;
    for (int i=0; i<16; ++i){
        Value* it = &v->as.list->data[i];
        if (it->kind != VAL_NUM) return false;
        out16[i] = it->as.num;
    }
    return true;
}

static Value make_mat4(const double* m){
    return make_vec(m, 16);
}

#define MAT(m,r,c) (m[(c)*4 + (r)])

static void mat4_identity_arr(double* m){
    for (int i=0;i<16;i++) m[i] = 0.0;
    MAT(m,0,0)=1.0; MAT(m,1,1)=1.0; MAT(m,2,2)=1.0; MAT(m,3,3)=1.0;
}

/* ---- vec constructors ---- */
static Value k_vec2(int n, const Value* a){
    if (n < 2 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM) return v_list_new();
    double v[2] = {a[0].as.num, a[1].as.num};
    return make_vec(v, 2);
}
static Value k_vec3(int n, const Value* a){
    if (n < 3 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM || a[2].kind != VAL_NUM) return v_list_new();
    double v[3] = {a[0].as.num, a[1].as.num, a[2].as.num};
    return make_vec(v, 3);
}
static Value k_vec4(int n, const Value* a){
    if (n < 4 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM || a[2].kind != VAL_NUM || a[3].kind != VAL_NUM) return v_list_new();
    double v[4] = {a[0].as.num, a[1].as.num, a[2].as.num, a[3].as.num};
    return make_vec(v, 4);
}

/* ---- vec2 ops ---- */
static Value k_vec2_add(int n, const Value* a){
    double v0[2], v1[2];
    if (n<2 || !read_vec(&a[0],2,v0) || !read_vec(&a[1],2,v1)) return v_list_new();
    double r[2] = {v0[0]+v1[0], v0[1]+v1[1]};
    return make_vec(r, 2);
}
static Value k_vec2_sub(int n, const Value* a){
    double v0[2], v1[2];
    if (n<2 || !read_vec(&a[0],2,v0) || !read_vec(&a[1],2,v1)) return v_list_new();
    double r[2] = {v0[0]-v1[0], v0[1]-v1[1]};
    return make_vec(r, 2);
}
static Value k_vec2_scale(int n, const Value* a){
    double v0[2];
    if (n<2 || !read_vec(&a[0],2,v0) || a[1].kind!=VAL_NUM) return v_list_new();
    double s = a[1].as.num;
    double r[2] = {v0[0]*s, v0[1]*s};
    return make_vec(r, 2);
}
static Value k_vec2_length(int n, const Value* a){
    double v0[2];
    if (n<1 || !read_vec(&a[0],2,v0)) return v_num(0);
    double len = sqrt(v0[0]*v0[0] + v0[1]*v0[1]);
    return v_num(len);
}
static Value k_vec2_normalize(int n, const Value* a){
    double v0[2];
    if (n<1 || !read_vec(&a[0],2,v0)) return v_list_new();
    double len = sqrt(v0[0]*v0[0] + v0[1]*v0[1]);
    if (len <= 0.0) return make_vec(v0, 2);
    double inv = 1.0 / len;
    double r[2] = {v0[0]*inv, v0[1]*inv};
    return make_vec(r, 2);
}

/* ---- vec3 ops ---- */
static Value k_vec3_add(int n, const Value* a){
    double v0[3], v1[3];
    if (n<2 || !read_vec(&a[0],3,v0) || !read_vec(&a[1],3,v1)) return v_list_new();
    double r[3] = {v0[0]+v1[0], v0[1]+v1[1], v0[2]+v1[2]};
    return make_vec(r, 3);
}
static Value k_vec3_sub(int n, const Value* a){
    double v0[3], v1[3];
    if (n<2 || !read_vec(&a[0],3,v0) || !read_vec(&a[1],3,v1)) return v_list_new();
    double r[3] = {v0[0]-v1[0], v0[1]-v1[1], v0[2]-v1[2]};
    return make_vec(r, 3);
}
static Value k_vec3_scale(int n, const Value* a){
    double v0[3];
    if (n<2 || !read_vec(&a[0],3,v0) || a[1].kind!=VAL_NUM) return v_list_new();
    double s = a[1].as.num;
    double r[3] = {v0[0]*s, v0[1]*s, v0[2]*s};
    return make_vec(r, 3);
}
static Value k_vec3_dot(int n, const Value* a){
    double v0[3], v1[3];
    if (n<2 || !read_vec(&a[0],3,v0) || !read_vec(&a[1],3,v1)) return v_num(0);
    double d = v0[0]*v1[0] + v0[1]*v1[1] + v0[2]*v1[2];
    return v_num(d);
}
static Value k_vec3_cross(int n, const Value* a){
    double v0[3], v1[3];
    if (n<2 || !read_vec(&a[0],3,v0) || !read_vec(&a[1],3,v1)) return v_list_new();
    double r[3] = {
        v0[1]*v1[2] - v0[2]*v1[1],
        v0[2]*v1[0] - v0[0]*v1[2],
        v0[0]*v1[1] - v0[1]*v1[0]
    };
    return make_vec(r, 3);
}
static Value k_vec3_length(int n, const Value* a){
    double v0[3];
    if (n<1 || !read_vec(&a[0],3,v0)) return v_num(0);
    double len = sqrt(v0[0]*v0[0] + v0[1]*v0[1] + v0[2]*v0[2]);
    return v_num(len);
}
static Value k_vec3_normalize(int n, const Value* a){
    double v0[3];
    if (n<1 || !read_vec(&a[0],3,v0)) return v_list_new();
    double len = sqrt(v0[0]*v0[0] + v0[1]*v0[1] + v0[2]*v0[2]);
    if (len <= 0.0) return make_vec(v0, 3);
    double inv = 1.0 / len;
    double r[3] = {v0[0]*inv, v0[1]*inv, v0[2]*inv};
    return make_vec(r, 3);
}
static Value k_vec3_distance(int n, const Value* a){
    double v0[3], v1[3];
    if (n<2 || !read_vec(&a[0],3,v0) || !read_vec(&a[1],3,v1)) return v_num(0);
    double dx=v0[0]-v1[0], dy=v0[1]-v1[1], dz=v0[2]-v1[2];
    return v_num( sqrt(dx*dx + dy*dy + dz*dz) );
}

/* ---- vec4 ops ---- */
static Value k_vec4_add(int n, const Value* a){
    double v0[4], v1[4];
    if (n<2 || !read_vec(&a[0],4,v0) || !read_vec(&a[1],4,v1)) return v_list_new();
    double r[4] = {v0[0]+v1[0], v0[1]+v1[1], v0[2]+v1[2], v0[3]+v1[3]};
    return make_vec(r, 4);
}
static Value k_vec4_sub(int n, const Value* a){
    double v0[4], v1[4];
    if (n<2 || !read_vec(&a[0],4,v0) || !read_vec(&a[1],4,v1)) return v_list_new();
    double r[4] = {v0[0]-v1[0], v0[1]-v1[1], v0[2]-v1[2], v0[3]-v1[3]};
    return make_vec(r, 4);
}
static Value k_vec4_scale(int n, const Value* a){
    double v0[4];
    if (n<2 || !read_vec(&a[0],4,v0) || a[1].kind!=VAL_NUM) return v_list_new();
    double s = a[1].as.num;
    double r[4] = {v0[0]*s, v0[1]*s, v0[2]*s, v0[3]*s};
    return make_vec(r, 4);
}
static Value k_vec4_dot(int n, const Value* a){
    double v0[4], v1[4];
    if (n<2 || !read_vec(&a[0],4,v0) || !read_vec(&a[1],4,v1)) return v_num(0);
    double d = v0[0]*v1[0] + v0[1]*v1[1] + v0[2]*v1[2] + v0[3]*v1[3];
    return v_num(d);
}

/* ---- mat4 ---- */
static Value k_mat4_identity(int n, const Value* a){
    (void)n; (void)a;
    double m[16];
    mat4_identity_arr(m);
    return make_mat4(m);
}

static Value k_mat4_mul(int n, const Value* a){
    double A[16], B[16];
    if (n<2 || !read_mat4(&a[0],A) || !read_mat4(&a[1],B)) return v_list_new();
    double R[16];
    for (int r=0; r<4; ++r){
        for (int c=0; c<4; ++c){
            double s=0.0;
            for (int k=0;k<4;++k) s += MAT(A,r,k) * MAT(B,k,c);
            MAT(R,r,c) = s;
        }
    }
    return make_mat4(R);
}

static Value k_mat4_translation(int n, const Value* a){
    if (n<3 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_NUM) return v_list_new();
    double m[16]; mat4_identity_arr(m);
    MAT(m,0,3) = a[0].as.num;
    MAT(m,1,3) = a[1].as.num;
    MAT(m,2,3) = a[2].as.num;
    return make_mat4(m);
}

static Value k_mat4_scale(int n, const Value* a){
    if (n<3 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_NUM) return v_list_new();
    double m[16]; mat4_identity_arr(m);
    MAT(m,0,0) = a[0].as.num;
    MAT(m,1,1) = a[1].as.num;
    MAT(m,2,2) = a[2].as.num;
    return make_mat4(m);
}

static Value k_mat4_rot_x(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_list_new();
    double ang = a[0].as.num;
    double c = cos(ang), s = sin(ang);
    double m[16]; mat4_identity_arr(m);
    MAT(m,1,1) = c;  MAT(m,1,2) = s;
    MAT(m,2,1) = -s; MAT(m,2,2) = c;
    return make_mat4(m);
}
static Value k_mat4_rot_y(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_list_new();
    double ang = a[0].as.num;
    double c = cos(ang), s = sin(ang);
    double m[16]; mat4_identity_arr(m);
    MAT(m,0,0) = c;  MAT(m,0,2) = s;
    MAT(m,2,0) = -s; MAT(m,2,2) = c;
    return make_mat4(m);
}
static Value k_mat4_rot_z(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_list_new();
    double ang = a[0].as.num;
    double c = cos(ang), s = sin(ang);
    double m[16]; mat4_identity_arr(m);
    MAT(m,0,0) = c;  MAT(m,0,1) = s;
    MAT(m,1,0) = -s; MAT(m,1,1) = c;
    return make_mat4(m);
}

static Value k_mat4_ypr(int n, const Value* a){
    if (n<3 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_NUM) return v_list_new();
    double yaw=a[0].as.num, pitch=a[1].as.num, roll=a[2].as.num;
    double cz = cos(roll), sz = sin(roll);
    double cx = cos(pitch), sx = sin(pitch);
    double cy = cos(yaw), sy = sin(yaw);

    double rz[16], rx[16], ry[16];
    mat4_identity_arr(rz); mat4_identity_arr(rx); mat4_identity_arr(ry);
    MAT(rz,0,0)=cz; MAT(rz,0,1)=sz; MAT(rz,1,0)=-sz; MAT(rz,1,1)=cz;
    MAT(rx,1,1)=cx; MAT(rx,1,2)=sx; MAT(rx,2,1)=-sx; MAT(rx,2,2)=cx;
    MAT(ry,0,0)=cy; MAT(ry,0,2)=sy; MAT(ry,2,0)=-sy; MAT(ry,2,2)=cy;

    double tmp[16], out[16];
    for (int r=0;r<4;++r){
        for (int c=0;c<4;++c){
            double s=0.0;
            for (int k=0;k<4;++k) s += MAT(rz,r,k) * MAT(rx,k,c);
            MAT(tmp,r,c) = s;
        }
    }
    for (int r=0;r<4;++r){
        for (int c=0;c<4;++c){
            double s=0.0;
            for (int k=0;k<4;++k) s += MAT(tmp,r,k) * MAT(ry,k,c);
            MAT(out,r,c) = s;
        }
    }
    return make_mat4(out);
}

static Value k_mat4_perspective(int n, const Value* a){
    if (n<4 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_NUM || a[3].kind!=VAL_NUM) return v_list_new();
    double fovy = a[0].as.num;
    double aspect = a[1].as.num;
    double zn = a[2].as.num;
    double zf = a[3].as.num;
    if (aspect == 0.0 || zn <= 0.0 || zf <= zn){
        fprintf(stderr, "math3d.mat4_perspective: invalid params\n");
        return v_list_new();
    }
    double f = 1.0 / tan(fovy * 0.5);
    double m[16]; memset(m, 0, sizeof(m));
    MAT(m,0,0) = f / aspect;
    MAT(m,1,1) = f;
    MAT(m,2,2) = zf / (zf - zn);
    MAT(m,2,3) = (-zn * zf) / (zf - zn);
    MAT(m,3,2) = 1.0;
    MAT(m,3,3) = 0.0;
    return make_mat4(m);
}

static Value k_mat4_look_at(int n, const Value* a){
    double eye[3], target[3], up[3];
    if (n<3 || !read_vec(&a[0],3,eye) || !read_vec(&a[1],3,target) || !read_vec(&a[2],3,up)) return v_list_new();
    double f[3] = {target[0]-eye[0], target[1]-eye[1], target[2]-eye[2]};
    double flen = sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    if (flen <= 0.0) return v_list_new();
    f[0]/=flen; f[1]/=flen; f[2]/=flen;
    double r[3] = {
        up[1]*f[2] - up[2]*f[1],
        up[2]*f[0] - up[0]*f[2],
        up[0]*f[1] - up[1]*f[0]
    };
    double rlen = sqrt(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]);
    if (rlen <= 0.0) return v_list_new();
    r[0]/=rlen; r[1]/=rlen; r[2]/=rlen;
    double u[3] = {
        f[1]*r[2] - f[2]*r[1],
        f[2]*r[0] - f[0]*r[2],
        f[0]*r[1] - f[1]*r[0]
    };
    double m[16];
    MAT(m,0,0)=r[0]; MAT(m,0,1)=u[0]; MAT(m,0,2)=f[0]; MAT(m,0,3)=0.0;
    MAT(m,1,0)=r[1]; MAT(m,1,1)=u[1]; MAT(m,1,2)=f[1]; MAT(m,1,3)=0.0;
    MAT(m,2,0)=r[2]; MAT(m,2,1)=u[2]; MAT(m,2,2)=f[2]; MAT(m,2,3)=0.0;
    MAT(m,3,0)=-(r[0]*eye[0] + r[1]*eye[1] + r[2]*eye[2]);
    MAT(m,3,1)=-(u[0]*eye[0] + u[1]*eye[1] + u[2]*eye[2]);
    MAT(m,3,2)=-(f[0]*eye[0] + f[1]*eye[1] + f[2]*eye[2]);
    MAT(m,3,3)=1.0;
    return make_mat4(m);
}

static Value k_transform_vec3(int n, const Value* a){
    double m[16], v[3];
    if (n<2 || !read_mat4(&a[0],m) || !read_vec(&a[1],3,v)) return v_list_new();
    double x=v[0], y=v[1], z=v[2], w=1.0;
    double rx = MAT(m,0,0)*x + MAT(m,0,1)*y + MAT(m,0,2)*z + MAT(m,0,3)*w;
    double ry = MAT(m,1,0)*x + MAT(m,1,1)*y + MAT(m,1,2)*z + MAT(m,1,3)*w;
    double rz = MAT(m,2,0)*x + MAT(m,2,1)*y + MAT(m,2,2)*z + MAT(m,2,3)*w;
    double rw = MAT(m,3,0)*x + MAT(m,3,1)*y + MAT(m,3,2)*z + MAT(m,3,3)*w;
    if (rw != 0.0){ rx/=rw; ry/=rw; rz/=rw; }
    double out[3] = {rx, ry, rz};
    return make_vec(out, 3);
}

static Value k_transform_vec4(int n, const Value* a){
    double m[16], v[4];
    if (n<2 || !read_mat4(&a[0],m) || !read_vec(&a[1],4,v)) return v_list_new();
    double x=v[0], y=v[1], z=v[2], w=v[3];
    double rx = MAT(m,0,0)*x + MAT(m,0,1)*y + MAT(m,0,2)*z + MAT(m,0,3)*w;
    double ry = MAT(m,1,0)*x + MAT(m,1,1)*y + MAT(m,1,2)*z + MAT(m,1,3)*w;
    double rz = MAT(m,2,0)*x + MAT(m,2,1)*y + MAT(m,2,2)*z + MAT(m,2,3)*w;
    double rw = MAT(m,3,0)*x + MAT(m,3,1)*y + MAT(m,3,2)*z + MAT(m,3,3)*w;
    double out[4] = {rx, ry, rz, rw};
    return make_vec(out, 4);
}

static const KirNativeDef FUNCS[] = {
    {"vec2", k_vec2}, {"vec3", k_vec3}, {"vec4", k_vec4},
    {"vec2_add", k_vec2_add}, {"vec2_sub", k_vec2_sub}, {"vec2_scale", k_vec2_scale},
    {"vec2_length", k_vec2_length}, {"vec2_normalize", k_vec2_normalize},
    {"vec3_add", k_vec3_add}, {"vec3_sub", k_vec3_sub}, {"vec3_scale", k_vec3_scale},
    {"vec3_dot", k_vec3_dot}, {"vec3_cross", k_vec3_cross},
    {"vec3_length", k_vec3_length}, {"vec3_normalize", k_vec3_normalize}, {"vec3_distance", k_vec3_distance},
    {"vec4_add", k_vec4_add}, {"vec4_sub", k_vec4_sub}, {"vec4_scale", k_vec4_scale}, {"vec4_dot", k_vec4_dot},
    {"mat4_identity", k_mat4_identity}, {"mat4_mul", k_mat4_mul},
    {"mat4_translation", k_mat4_translation}, {"mat4_scale", k_mat4_scale},
    {"mat4_rotation_x", k_mat4_rot_x}, {"mat4_rotation_y", k_mat4_rot_y}, {"mat4_rotation_z", k_mat4_rot_z},
    {"mat4_rotation_yaw_pitch_roll", k_mat4_ypr},
    {"mat4_perspective", k_mat4_perspective}, {"mat4_look_at", k_mat4_look_at},
    {"transform_vec3", k_transform_vec3}, {"transform_vec4", k_transform_vec4},
};

void math3d_init(void){
    kir_native_register_module("math3d", FUNCS, (int)(sizeof(FUNCS)/sizeof(FUNCS[0])));
    kir_native_mark_bytecode_module_safe("math3d");
}
