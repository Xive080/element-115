#!/bin/bash
# Harness P3 (Mac arm64): compila gfx_d3d 2D + universal REAL + db_*.cpp + rt2/trace/main2; stubs autogenerados (aborta con nombre).
set -e
H=$(cd "$(dirname "$0")/.." && pwd); Z=$H/../zdec
# BACKEND (variable de COMPILACION): vk | trace | gles.   TARGET=ndk -> Android arm64 (NDK 28): por defecto gles (h0/gles); BACKEND=vk enlaza libv9.a (h0/vk),
# BACKEND=trace sin render.   Mac: por defecto trace; BACKEND=vk renderiza con Vulkan sobre MoltenVK (offscreen; WIN=1 anade ventana Cocoa, P4_WINDOW=1 al ejecutar).
# En EJECUCION el selector es P3_BACKEND=trace (desactiva el backend enlazado). Con BACKEND=vk NO se enlaza la MojoShader de GLES (libv9.a ya trae la suya
# con el perfil spirv: dos MojoShader duplican los simbolos MOJOSHADER_*) ni gles/d3d9_gles.cpp. Salidas: out (mac trace), out_vk (mac vk), out_ndk, out_ndk_vk, out_app, out_app_vk.
# Otras: EXTRA_FL (banderas del harness; -funsigned-char reproduce el char de Android arm64), OPT (harness, -O0 por defecto), VOPT (libv9, -O1 -g por defecto), FORCE=1 recompila todo.
GLEXTRA=""; GLES_BE=""; VK_BE=""; SUF=${OSUF:+_$OSUF}; [ -n "$H5G" ] && SUF="${SUF}_h5"; [ -n "$H5G" ] && EXTRA_FL="$EXTRA_FL -DH5G"; [ -n "$H5G" ] && [ -n "$H5FX" ] && EXTRA_FL="$EXTRA_FL -DH5FX"   # H5FX=1: OLA 6c efectos (con H5G)
H6SND=${H6SND-1}; [ "$H6SND" = 0 ] && H6SND=; [ -z "$H5G" ] && H6SND=   # OLA 14: sonido por defecto con H5G; H6SND=0 lo quita al compilar (H6A=0 en ejecucion)
[ -n "$H5G" ] && [ -n "$H6SND" ] && EXTRA_FL="$EXTRA_FL -DH6SND"   # OLA 6a sonido (con H5G)
H13=${H13-1}; H12GLASS=${H12GLASS-1}; [ "$H12GLASS" = 0 ] && H12GLASS=   # OLA 15A: HUD original (H13) y cristal (H12GLASS) por defecto con H5G; H13=0 / H12GLASS=0 los quitan
# H5Q: TUs extra de qcommon/universal para H5G (rutas relativas a zdec/src sin .cpp)
H5Q=${H5Q:-"qcommon/cm_world qcommon/cmd qcommon/msg qcommon/msg_mp qcommon/huffman qcommon/sv_msg_write qcommon/sv_msg_write_mp qcommon/dobj_management qcommon/com_clients universal/UserInfo universal/reliablemsg universal/com_constantconfigstrings demo/demo_common qcommon/net_chan_mp qcommon/cm_load qcommon/cm_load_obj qcommon/com_bsp_load_obj universal/com_stringtable qcommon/com_gamemodes qcommon/cm_mapkit qcommon/cm_noperks qcommon/md4 turret/turret_placement DynEntity/DynEntity_server universal/mem_largelocal universal/com_stringtable_obj universal/surfaceflags glass/glass_server DynEntity/DynEntity_load_obj DynEntity/DynEntity_coll physics/destructible glass/glass_load_obj ik/ik ik/ik_import ik/ik_process ik/ik_layers flame/flame_damage flame/flame_system flame/flame_class_chunk flame/flame_class_drips flame/flame_class_fire flame/flame_class_smoke flame/flame_class_stream qcommon/dvar_cmds flame/flame_physics client_mp/sv_client_mp client_mp/g_client_mp"}   # OSUF=x: salida en out_vk_x (variantes con EXTRA_FL distinto, p.ej. -funsigned-char)
if [ "$TARGET" = ndk ]; then
  O=$H/harness2/out_ndk; [ -n "$APP" ] && O=$H/harness2/out_app; CXX=~/Library/Android/sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++
  LIBS="-lz -llog -static-libstdc++"; [ -n "$APP" ] && LIBS="$LIBS -landroid"
  # char con signo como en x86 y en el Mac (la referencia probada): el codigo decompilado hace (char)x esperando signo
  # (p.ej. ps->movementDir = (char)moveyaw -> 248 en vez de -8 y assert en BG_PlayerToEntitySetTrajectory). OLA 5.
  EXTRA_FL="$EXTRA_FL -fsigned-char"
  NPFX="-ffile-prefix-map=$(cd $H/.. && pwd)/="; EXTRA_FL="$EXTRA_FL $NPFX"   # OLA 15F: sin rutas /Users/ (__FILE__) en libmain.so; tambien miniaudio (abajo) y libv9 (build_vk.sh)
  if [ "$BACKEND" = vk ]; then
    VK_BE=1; O=$O"_vk$SUF"; VWSI=none; [ -n "$APP" ] && VWSI=android
    # -landroid -llog: libv9.a (vk_wsi_android.cpp) usa ANativeWindow_*/__android_log_*; tambien para el ejecutable de consola
    LIBS="$O/vk/libv9.a -landroid $LIBS"; case "$LIBS" in *-llog*) ;; *) LIBS="$LIBS -llog";; esac
  elif [ "$BACKEND" != trace ]; then
    $H/gles/build_mojoshader.sh ndk >/dev/null || exit 1
    GLEXTRA="-DD3D9_GLES -DMOJOSHADER_NO_VERSION_INCLUDE=1 -I $H/../third_party/mojoshader"; GLES_BE=1; LIBS="$H/gles/out_ndk/ms/libmojoshader.a -lEGL -lGLESv3 $LIBS"
  fi
else
  O=$H/harness2/out$SUF; CXX="clang++ -target arm64-apple-macos13"; LIBS="-lz"
  if [ "$BACKEND" = vk ]; then
    VK_BE=1; O=$H/harness2/out_vk$SUF; VWSI=none; MACFW="-framework Foundation -framework QuartzCore -framework Metal"
    [ -n "$WIN" ] && { VWSI=mac; MACFW="$MACFW -framework Cocoa"; }
    LIBS="$O/vk/libv9.a $MACFW -lz"
  fi
fi
if [ -n "$VK_BE" ]; then
  # libv9.a (+MojoShader spirv dentro). Se construye en <salida>/vk (V9_OUT) para no pisar el out_<mac|ndk> de las pruebas de vk/.
  VT=mac; [ "$TARGET" = ndk ] && VT=ndk
  mkdir -p $O; V9_OUT=$O/vk WSI=$VWSI OPT="${VOPT:--O1 -g}" $H/vk/build_vk.sh $VT >/dev/null || { echo "FALLA libv9.a"; exit 1; }
  GLEXTRA="-DD3D9_VK -DVK_ENABLE_BETA_EXTENSIONS"
  # Cocoa choca con compat/Windows.h (BOOL, IUnknown, prelude): el ayudante de ventana se compila APARTE, sin el prelude ni -I compat
  if [ "$VT" = mac ] && [ -n "$WIN" ]; then
    mkdir -p $O/winobj; clang++ -target arm64-apple-macos13 -std=c++20 -O1 -g -w -fobjc-arc -x objective-c++ -c $H/vk/tests/t_window_win.mm -o $O/winobj/t_window_win.o || exit 1
    LIBS="$O/winobj/t_window_win.o $LIBS"; EXTRA_FL="$EXTRA_FL -DH0_MACWIN"
    if [ -n "$H5G" ]; then clang++ -target arm64-apple-macos13 -std=c++20 -O1 -g -w -fobjc-arc -x objective-c++ -c $H/harness2/h10_macin.mm -o $O/winobj/h10_macin.o || exit 1; LIBS="$O/winobj/h10_macin.o $LIBS"; fi   # OLA 10: raton/teclado del menu de ajustes
  fi
fi
mkdir -p $O/obj $O/log
FL="-std=c++20 ${OPT:--O0} -g -I $H/harness2 -I $O/gen -I $H/harness/shim -I $H/compat -include $H/compat/prelude.h -I $Z -I $Z/src -I $Z/src/libs -DBO1_MP -DH0_HARNESS $EXTRA_FL ${APP:+-DBO1_APP -fPIC} -DD3D9_TRACE $GLEXTRA -fpermissive -w -Wno-c++11-narrowing -fno-strict-aliasing -ferror-limit=0"
GFX="r_rendercmds rb_backend rb_shade r_shade r_state r_state_utils r_buffers r_image r_image_load_common r_image_load_obj r_material r_font r_rendertarget rb_resource rb_stats r_utils r_dvars r_scene r_shader_constant_set r_init rb_state r_sky r_reflection_probe r_warn rb_stream r_image_wavelet r_foliage rb_draw3d r_draw_bsp r_draw_material r_vertexstream2 rb_tess r_state_utils"
UNI="dvar q_shared com_math com_shared com_math_anglevectors com_pack q_parse"
srcs=""
for b in $GFX; do srcs="$srcs $Z/src/gfx_d3d/$b.cpp"; done
for b in $UNI; do srcs="$srcs $Z/src/universal/$b.cpp"; done
for b in cm_trace cm_test cm_mesh cm_tracebox cm_staticmodel; do srcs="$srcs $Z/src/qcommon/$b.cpp"; done   # colisiones reales (CM_BoxTrace/capsula)
srcs="$srcs $Z/src/database/db_*.cpp $H/harness2/trace_defaults.cpp $H/harness2/rt2.cpp $H/harness2/trace.cpp $H/harness2/main2.cpp $H/harness2/iwd.cpp $H/harness2/world_h4.cpp $H/harness2/memguard.cpp"
srcs="$srcs $Z/src/stringed/stringed_hooks.cpp $Z/src/ik/ik_math.cpp"
# H4: movimiento ORIGINAL del jugador (PmoveSingle/PM_WalkMove/PM_StepSlideMove/salto); el pegamento minimo esta en pmove_h4.cpp
for b in bg_pmove bg_slidemove bg_jump bg_misc bg_mantle bg_dtp bg_perks; do srcs="$srcs $Z/src/bgame/$b.cpp"; done
srcs="$srcs $Z/src/physics/phys_traverse.cpp $H/harness2/pmove_h4.cpp"   # phys_traverse: colgeom_visitor_t (proximidad de PM_trace)
# P5: UI real (menus de zona)
UIS="ui/ui_shared ui/ui_main ui/ui_atoms ui/ui_localvars ui/ui_feeders ui/ui_utils ui_mp/ui_feeders_mp universal/com_expressions universal/com_expressions_eval universal/com_loadutils client/screen_placement client_mp/cl_cgame_mp client_mp/cl_scrn_mp client/cl_main client_mp/cl_main_mp"
for b in $UIS; do srcs="$srcs $Z/src/$b.cpp"; done
srcs="$srcs $H/harness2/ui_p5.cpp"
[ -n "$VK_BE" ] && srcs="$srcs $H/harness2/h6_loading.cpp"   # OLA 6b: pantalla "Loading shaders" (precompila pipelines.keys)
srcs="$srcs $H/harness2/h7_gfx.cpp $Z/src/gfx_d3d/rb_postfx.cpp"   # OLA 7g: fidelidad grafica (mips altos, vision set, RB_BloomLDR original)
for f in $H/harness2/h7c_gfx.cpp $Z/src/gfx_d3d/rb_fog.cpp; do case " $H8_XSRC $EXTRA " in *"$(basename $f)"*) ;; *) srcs="$srcs $f";; esac; done   # OLA 7c: brushmodels/smodels/fov/brazos/HUD + niebla (R_SetFrameFog original); sin duplicar si ya van en H8_XSRC/EXTRA
case " $H8_XSRC $EXTRA " in *h7e_light.cpp*) ;; *) srcs="$srcs $H/harness2/h7e_light.cpp";; esac   # OLA 7e: luz de la sala + estado de cliente (luces de script, vision por planta, script_model de cliente)
for f in h9_shadow.cpp h9_reflect.cpp h9_video.cpp; do [ -f $H/harness2/$f ] || continue; case " $H8_XSRC $EXTRA " in *"$f"*) ;; *) srcs="$srcs $H/harness2/$f";; esac; done   # OLA 9: sombras de focos, sondas de reflejo, pantallas de la caja
# H5a0: VM GSC original (clientscript) + pegamento/prueba aislada (h5_vm.cpp, H5_VM=1). Lista de nombres de builtins generada de las tablas reales.
for b in cscr_animtree cscr_compiler cscr_debugger cscr_evaluate cscr_instance cscr_main cscr_memorytree cscr_parser cscr_parsetree cscr_readwrite cscr_save cscr_stringlist cscr_tempmemory cscr_variable cscr_vm cscr_yacc scr_const; do srcs="$srcs $Z/src/clientscript/$b.cpp"; done
srcs="$srcs $H/harness2/h5_vm.cpp"
# H5a (H5G=1, salida aparte out_*_h5): capa de juego/servidor ORIGINAL (server, server_mp, game, game_mp, game_sp, xanim, resto de bgame,
# parte de qcommon) + arranque del servidor dedicado en h5_game.cpp. No cambia el build de H4/P4/app (otra carpeta de salida).
if [ -n "$H5G" ]; then
  H5X=" g_sp_savegame xanim_load_obj xmodel_load_obj xmodel_load_phys_collmap "   # guardado SP (layout x86) y cargadores sin fastfile: no se usan
  for d in server server_mp game game_mp game_sp xanim; do for f in $Z/src/$d/*.cpp; do case "$H5X" in *" $(basename $f .cpp) "*) ;; *) srcs="$srcs $f";; esac; done; done
  for f in $Z/src/bgame/*.cpp; do case " bg_pmove bg_slidemove bg_jump bg_misc bg_mantle bg_dtp bg_perks " in *" $(basename $f .cpp) "*) ;; *) srcs="$srcs $f";; esac; done
  for b in $H5Q; do srcs="$srcs $Z/src/$b.cpp"; done
  srcs="$srcs $H/harness2/h10_settings.cpp"   # OLA 10: menu de ajustes original + perfil de calidad + config
  for f in $H/harness2/h11_net.cpp $H/harness2/h11_net_cl.cpp; do case " $EXTRA " in *"$(basename $f)"*) ;; *) srcs="$srcs $f";; esac; done   # OLA 11: red co-op (inerte sin H11_HOST/H11_CONNECT)
  srcs="$srcs $H/harness2/h5_game.cpp $H/harness2/h5_weapon.cpp $H/harness2/h5_play.cpp $H/harness2/h8_doors.cpp $H/harness2/h12_barrier.cpp ${H8_XSRC}"
  [ -n "$H12GLASS" ] && srcs="$srcs $Z/src/glass/glass_client.cpp $Z/src/glass/glass_renderer.cpp $Z/src/glass/glass_shard.cpp $Z/src/glass/glass_allocator.cpp $H/harness2/h12_glasscl.cpp"   # OLA 12: cliente de cristal original (fragmentos GlassShard)   # OLA 8 (H8_XSRC: fuentes extra para pruebas)   # h5_weapon: H5e (arma, viewmodel, registro [H5E])
  srcs="$srcs $Z/src/cgame/cg_weapons.cpp"   # H5e: viewmodel/animacion de arma del cliente ORIGINAL
  if [ "${H13:-0}" != 0 ]; then   # OLA 13: HUD ORIGINAL (h13_hud.cpp + TUs de cgame enteras por envoltorio h13/h13w_*.cpp; H13=0 al compilar lo quita)
    for w in $H/harness2/h13/h13w_*.cpp; do s=$Z/$(sed -n 's/^#include <\(src\/.*\)>/\1/p' $w); [ $s -nt $w ] && touch $w; srcs="$srcs $w"; done
    srcs="$srcs $H/harness2/h13_hud.cpp"
    mkdir -p $O/gen; python3 -I $H/harness2/h5_extract.py --noinc $Z/src/ui/ui_shared_obj.cpp "MenuList *__cdecl UI_LoadMenus(const char *menuFile, int imageTrack)||MenuList *__cdecl UI_LoadMenus_FastFile(const char *menuFile)" $O/gen/h13_uiload.inc || exit 1   # el resto de ui_shared_obj es el cargador de .menu sueltos
    python3 -I $H/harness2/h5_extract.py --noinc $Z/src/cgame_mp/cg_servercmds_mp.cpp "void __cdecl CG_MenuShowNotify(int localClientNum, int menuToShow)" $O/gen/h13_notify.inc || exit 1
    python3 -I $H/harness2/h5_extract.py --noinc $Z/src/cgame/cg_playerstate.cpp "void __cdecl CG_DamageFeedback(int localClientNum, int yawByte, int pitchByte, int damage)" $O/gen/h13_dmgfb.inc || exit 1   # indicador de dano direccional + destello (CG_TransitionPlayerState)   # fundidos del HUD (municion, dpad, salud); el resto de la TU son comandos de red
    awk '/^void __cdecl CG_Init\(|^void __cdecl CG_RegisterGraphics\(/{f=1} f&&/^}/{f=0} f&&/^ *(cgMedia\.[A-Za-z_0-9\[\]]+ = )?(Material_RegisterHandle|CL_RegisterFont)\(/' $Z/src/cgame_mp/cg_main_mp.cpp > $O/gen/h13_media.tmp   # materiales/fuentes del HUD de CG_Init + CG_RegisterGraphics (sin FX ni modelos)
    cmp -s $O/gen/h13_media.tmp $O/gen/h13_media.inc && rm $O/gen/h13_media.tmp || mv $O/gen/h13_media.tmp $O/gen/h13_media.inc
  fi
  srcs="$srcs $Z/src/gfx_d3d/r_model_skin.cpp $Z/src/gfx_d3d/r_xsurface.cpp"   # H5b: deformado en CPU (C portable; r_model_skin_sse.cpp no)
  srcs="$srcs $Z/src/gfx_d3d/rb_light.cpp $Z/src/gfx_d3d/r_pointlights.cpp $Z/src/gfx_d3d/r_primarylights.cpp $Z/src/gfx_d3d/r_draw_shadowablelight.cpp $H/harness2/h5_light.cpp"   # OLA 6b: lightgrid ORIGINAL -> luz de modelos
  if [ -n "$H6SND" ]; then   # OLA 6a: sistema de sonido ORIGINAL (snd*.cpp) con driver portable propio (h5_audio*.cpp + miniaudio) en vez de XAudio2/snd_stream
    for b in snd snd_bank snd_bo1_trace snd_db snd_debug snd_dsp snd_dvar snd_globals snd_local snd_log snd_occlusion snd_public_async snd_public_async_q snd_radverb snd_utils; do srcs="$srcs $Z/src/sound/$b.cpp"; done
    srcs="$srcs $H/harness2/h5_audio.cpp $H/harness2/h5_audio_cl.cpp"
    mkdir -p $O/gen; python3 -I $H/harness2/h5_extract.py --noinc $Z/src/cgame_mp/cg_main_mp.cpp "void __cdecl CG_RegisterSounds()||void __cdecl CG_RegisterSurfaceTypeSounds(const char *pszType, unsigned int *sound)||void __cdecl CG_RegisterImpactTypeSounds(const char *pszType, unsigned int *sound)" $O/gen/h6a_cgsnd1.inc || exit 1   # OLA 14: sonidos de cgMedia (golpes, impactos, whizby)
    python3 -I $H/harness2/h5_extract.py --noinc $Z/src/cgame/cg_sound.cpp "void __cdecl CG_SoundWhizby(||char __cdecl CG_SoundWhizbyPath(const float *p1, const float *p2, const float *p3, float r, float *start, float *end)" $O/gen/h6a_cgsnd2.inc || exit 1
    cat $O/gen/h6a_cgsnd1.inc $O/gen/h6a_cgsnd2.inc | sed -E 's/^(void|char) __cdecl CG_(RegisterSounds|RegisterSurfaceTypeSounds|RegisterImpactTypeSounds|SoundWhizby|SoundWhizbyPath)\(/__attribute__((weak)) &/' > $O/gen/h6a_cgsnd.inc.new; cmp -s $O/gen/h6a_cgsnd.inc.new $O/gen/h6a_cgsnd.inc || mv $O/gen/h6a_cgsnd.inc.new $O/gen/h6a_cgsnd.inc; rm -f $O/gen/h6a_cgsnd.inc.new   # weak: con H13=1 cg_main_mp.cpp entera ya las define
    [ -f $O/obj/h5_audio_cl.o ] && [ $O/gen/h6a_cgsnd.inc -nt $O/obj/h5_audio_cl.o ] && rm -f $O/obj/h5_audio_cl.o
  fi
  if [ -n "$H5FX" ]; then   # OLA 6c: efectos ORIGINALES (EffectsCore sin el cargador/conversor de ficheros sueltos ni el perfilador) + codemesh
    for f in $Z/src/EffectsCore/*.cpp; do case " fx_convert fx_load_obj fx_profile " in *" $(basename $f .cpp) "*) ;; *) srcs="$srcs $f";; esac; done
    srcs="$srcs $Z/src/gfx_d3d/r_drawsurf.cpp $Z/src/gfx_d3d/r_meshdata.cpp $Z/src/gfx_d3d/r_sprite.cpp $Z/src/gfx_d3d/r_pretess.cpp $Z/src/gfx_d3d/r_material_consts.cpp $H/harness2/h5_fx.cpp"
    srcs="$srcs $Z/src/gfx_d3d/r_marks.cpp"   # OLA 7d: marcas y decals ORIGINALES (R_MarkFragments_*, R_BoxSurfaces); en ejecucion H7D_MARKS=0 las quita
    srcs="$srcs $Z/src/gfx_d3d/r_draw_method.cpp $Z/src/gfx_d3d/r_char_tech_util.cpp"   # OLA 7d: R_SetDefaultLitTechTypes/R_UpdateDrawMethod (tecnica de luz de marcas y decals) y TechLit_NeedsCharredTech
    mkdir -p $O/gen; python3 -I $H/harness2/h5_extract.py --noinc $Z/src/EffectsCore/fx_load_obj.cpp "const FxEffectDef *__cdecl FX_Register(char *name)||const FxEffectDef *__cdecl FX_Register_FastFile(const char *name)" $O/gen/h5_fx_reg.inc || exit 1
    python3 -I $H/harness2/h5_extract.py --noinc $Z/src/gfx_d3d/r_material_load_obj.cpp "bool __cdecl Material_HasImpactMaskTech(const Material *material)||bool __cdecl Material_IsForSeeThruBulletHoleDecal(const Material *mat)||bool __cdecl Material_IsSeeThruParallaxDecal(const Material *mat)||void __cdecl Material_GetVertexShaderName(char *dest, const MaterialPass *pass, int destsize)||int __cdecl Material_ComparePixelConsts(const Material *mtl0, const Material *mtl1, unsigned __int8 techType)||void __cdecl Material_SortInternal(Material **sortedMaterials, unsigned int materialCount)||unsigned int __cdecl R_DrawSurfStandardPrepassSortKey(const Material *material)||bool __cdecl Material_Compare(const Material *mtl0, const Material *mtl1)||unsigned int __cdecl R_DrawSurfPrimarySortKey(const Material *material)" $O/gen/h5_fx_msort.inc || exit 1
    python3 -I $H/harness2/h5_extract.py --noinc $Z/src/cgame/cg_event.cpp "int __cdecl CG_GetBoneIndex(||unsigned int __cdecl CG_PlayBoltedEffect(" $O/gen/h5_fx_cgev.inc || exit 1
    python3 -I $H/harness2/h5_extract.py --noinc $Z/src/cgame/cg_effects_load_obj.cpp "FxImpactTable *__cdecl CG_RegisterImpactEffects_FastFile()" $O/gen/h5_fx_impact.inc || exit 1
  fi
  mkdir -p $O/gen; python3 -I $H/harness2/h5_extract.py --noinc $Z/src/cgame_mp/cg_ents_mp.cpp "unsigned int __cdecl Com_GetPrimaryLightCount()||const ComPrimaryLight *__cdecl Com_GetPrimaryLight(unsigned int primaryLightIndex)" $O/gen/h5_light_prim1.inc || exit 1
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/qcommon/com_bsp.cpp "char __cdecl Com_CanPrimaryLightAffectPoint(const ComPrimaryLight *light, const float *point)" $O/gen/h5_light_prim2.inc || exit 1
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/gfx_d3d/r_bsp_load_obj.cpp "void __cdecl R_InitPrimaryLights(GfxLight *primaryLights)" $O/gen/h5_light_prim3.inc || exit 1
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/gfx_d3d/r_light.cpp "GfxLightDef *__cdecl R_RegisterLightDef(const char *name)||GfxLightDef *__cdecl R_RegisterLightDef_FastFile(const char *name)" $O/gen/h5_light_prim4.inc || exit 1
  cat $O/gen/h5_light_prim1.inc $O/gen/h5_light_prim2.inc $O/gen/h5_light_prim4.inc $O/gen/h5_light_prim3.inc > $O/gen/h5_light_prim.inc.new; cmp -s $O/gen/h5_light_prim.inc.new $O/gen/h5_light_prim.inc || mv $O/gen/h5_light_prim.inc.new $O/gen/h5_light_prim.inc; rm -f $O/gen/h5_light_prim.inc.new
  # funciones de common.cpp que el servidor usa, extraidas tal cual (el resto de common.cpp trae cliente/FS/Com_Frame)
  H5COMF="void Com_InitDvars()||void __cdecl Com_SetScriptSettings()"
  for g in "int __cdecl Com_AddToString(const char *add, char *msg, int len, int maxlen, int mayAddQuotes)" "char __cdecl Com_GetDecimalDelimiter()" \
    "int __cdecl Com_GetFirstFreeEntityNum()" "int __cdecl Com_GetMaxActorCorpses()" "void __cdecl Com_FreeWeaponInfoMemory(int source)" \
    "void __cdecl Com_SetWeaponInfoMemory(int source)" "bool __cdecl Com_IsSPFrontEndLevel(const char *mapName)" "bool __cdecl Com_IsSPFrontEndRunning()" \
    "bool __cdecl Com_IsSPLevel()" "bool __cdecl Com_IsZombieMap(const char *mapName)" "bool __cdecl Com_IsMenuLevel(const char *name)" \
    "int __cdecl Com_GetPrivateClients()" "XAnimTree_s *__cdecl Com_XAnimCreateSmallTree(XAnim_s *anims)" "void __cdecl Com_XAnimFreeSmallTree(XAnimTree_s *animtree)" \
    "void __cdecl Com_LocalizedFloatToString(float f, char *buffer, unsigned int maxlen, unsigned int numDecimalPlaces)" "double __cdecl Com_GetTimescaleForSnd()" "double __cdecl Com_GetTimeScale()" "void __cdecl Com_SetTimeScale(float timescale)" \
    "void Com_DPrintf(int channel, const char *fmt, ...)" "const char *__cdecl Com_DisplayName(const char *name, const char *clanAbbrev, int type)" \
    "const char *__cdecl CS_DisplayName(const clientState_s *cs, int type)"; do H5COMF="$H5COMF||$g"; done
  mkdir -p $O/gen; python3 -I $H/harness2/h5_extract.py $Z/src/qcommon/common.cpp "$H5COMF" $O/gen/h5_cominitdvars.inc g_dedicatedEnumNames weaponInfoSource com_codeTimeScale asc_CD51B0 bo1_writeconfig bo1_mod_pcore bo1_mod_priority || exit 1
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/universal/com_files.cpp "void __cdecl Com_GetBspFilename(char *filename, unsigned int size, const char *mapname)" $O/gen/h5_comfiles.inc || exit 1
  # DynEntity_client.cpp trae la fisica Nitrous del cliente (sin portar): solo los dos accesores que usa el servidor
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/DynEntity/DynEntity_client.cpp "const DynEntityDef *__cdecl DynEnt_GetEntityDef(unsigned __int16 dynEntId, DynEntityDrawType drawType)||DynEntityColl *__cdecl DynEnt_GetEntityColl(DynEntityCollType collType, unsigned __int16 dynEntId)" $O/gen/h5_dynentcl.inc || exit 1
  # phys_collision.cpp es la fisica Nitrous (sin portar): solo el mapa AVL generico (g_mover.cpp mover_info, pathnode.cpp)
  # Funciones sueltas de TUs de cliente/carga que el servidor llama (cg_camera, cg_event, xmodel_load_obj, snd_utils)
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/cgame/cg_camera.cpp "void __cdecl RotatePoint(float *point, const float (*mat)[3])" $O/gen/h5_misc1.inc || exit 1
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/cgame/cg_event.cpp "bool __cdecl Com_IsRagdollTrajectory(const trajectory_t *trajectory)" $O/gen/h5_misc2.inc || exit 1
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/xanim/xmodel_load_obj.cpp "int __cdecl XModelGetStaticBounds(const XModel *model, float *mins, float *maxs)" $O/gen/h5_misc3.inc || exit 1
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/physics/phys_main.cpp "cdl_proftimer::cdl_proftimer()" $O/gen/h5_misc5.inc || exit 1   # constructor estatico de los temporizadores de flame_physics
  if [ -n "$H6SND" ]; then [ -f $O/gen/h5_misc4.inc ] && [ ! -s $O/gen/h5_misc4.inc ] || : > $O/gen/h5_misc4.inc   # OLA 6a: snd_utils.cpp entero
  else python3 -I $H/harness2/h5_extract.py --noinc $Z/src/sound/snd_utils.cpp "int __cdecl SND_HashName(const char *name)" $O/gen/h5_misc4.inc || exit 1; fi
  # H5b: asignador de arboles de animacion (Com_XAnimCreateSmallTree -> CG_AllocAnimTree = MT_Alloc) y matematica de huesos de TUs de cliente
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/cgame_mp/cg_ents_mp.cpp "char *__cdecl CG_AllocAnimTree(int size)" $O/gen/h5_misc6.inc || exit 1
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/cgame_mp/cg_pose_mp.cpp "void __cdecl QuatMultiplyEquals(const float *in, float *inout)" $O/gen/h5_misc7.inc || exit 1
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/cgame_mp/cg_pose_mp.cpp "void __cdecl MatrixTransformVectorQuatTrans(const float *in, const DObjAnimMat *mat, float *out)||void __cdecl BG_Player_DoControllers(const CEntPlayerInfo *player, const DObj *obj, int *partBits)" $O/gen/h5_weapon_pose.inc || exit 1   # H5e: trazas por hueso (DObjTraceline, G_PlayerController)
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/cgame_mp/cg_main_mp.cpp "void *__cdecl Hunk_AllocXAnimClient(unsigned int size)||const playerState_s *__cdecl CG_GetPredictedPlayerState(int localClientNum)" $O/gen/h5_weapon_cgmain.inc || exit 1   # H5e: lo que el viewmodel usa de cg_main_mp
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/cgame/cg_drawtools.cpp "void __cdecl ConvertQuatToMat(const DObjAnimMat *mat, float (*axis)[3])||double __cdecl Vec2NormalizeTo(const float *v, float *out)" $O/gen/h5_misc8.inc || exit 1
  # H5b: puente de render del servidor (H5_VIEW): cuaternion -> DObjSkelMat como R_SkinXModelCmd (definidas en fx_marks.cpp, efectos sin portar)
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/EffectsCore/fx_marks.cpp "void __cdecl ConvertQuatToSkelMat(const DObjAnimMat *mat, DObjSkelMat *skelMat)||void __cdecl ConvertQuatToInverseSkelMat(const DObjAnimMat *mat, DObjSkelMat *skelMat)" $O/gen/h5_misc10.inc || exit 1
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/gfx_d3d/r_dobj_skin.cpp "void __cdecl R_TransformSkelMat(const float *in1, const float4 *in2, float *out)" $O/gen/h5_misc11.inc || exit 1   # r_model_skin (pesos)
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/client/cl_keys.cpp "bool __cdecl Key_IsValidGamePadChar(char key)" $O/gen/h5_misc9.inc || exit 1   # Cmd_TokenizeString (connect del bot)
  python3 -I $H/harness2/h5_extract.py --noinc $Z/src/physics/phys_collision.cpp "generic_avl_map_node_t *__cdecl generic_avl_map_add(||void *__cdecl generic_avl_map_destroy(" $O/gen/h5_physavl.inc || exit 1
fi
mkdir -p $O/gen; python3 -I $H/harness2/h5_genbuiltins.py $Z/src $O/gen/h5_builtins.inc.new >/dev/null && { cmp -s $O/gen/h5_builtins.inc.new $O/gen/h5_builtins.inc || mv $O/gen/h5_builtins.inc.new $O/gen/h5_builtins.inc; rm -f $O/gen/h5_builtins.inc.new; }
if [ -n "$H5G" ] && [ -n "$H6SND" ]; then   # OLA 6a: salida de audio (miniaudio 0.11.21, third_party) compilada aparte, en C y sin el prelude de compat
  mkdir -p $O/maobj; MAO=$O/maobj/h5_audio_ma.o
  if [ ! -f $MAO ] || [ $H/harness2/h5_audio_ma.c -nt $MAO ]; then ${CXX/clang++/clang} -x c -std=c11 -O2 -w ${APP:+-fPIC} $NPFX -I $H/../third_party/miniaudio -c $H/harness2/h5_audio_ma.c -o $MAO || { echo "FAIL h5_audio_ma"; exit 1; }; fi
  LIBS="$MAO $LIBS"; if [ "$TARGET" = ndk ]; then LIBS="$LIBS -ldl -lm"; else LIBS="$LIBS -framework CoreAudio -framework AudioToolbox -framework CoreFoundation"; fi
fi
[ -n "$GLES_BE" ] && srcs="$srcs $H/gles/d3d9_gles.cpp"
[ -n "$EXTRA" ] && srcs="$srcs $EXTRA"
objs=""; fail=0
for f in $srcs; do
  [ -f $f ] || continue
  b=$(basename $f .cpp); o=$O/obj/$b.o; objs="$objs $o"
  if [ -n "$JOBS" ]; then while [ $(jobs -rp | wc -l) -ge $JOBS ]; do sleep 0.2; done; fi   # OLA 15A: JOBS=n limita los clang en paralelo (sin JOBS, todos a la vez como antes)
  if [ ! -f $o ] || [ $f -nt $o ] || [ $O/gen/h5_builtins.inc -nt $o -a $b = h5_vm ] || [ -f $O/gen/h5_cominitdvars.inc -a $O/gen/h5_cominitdvars.inc -nt $o -a $b = h5_game ] || [ -f $O/gen/h5_comfiles.inc -a $O/gen/h5_comfiles.inc -nt $o -a $b = h5_game ] || [ -f $O/gen/h5_dynentcl.inc -a $O/gen/h5_dynentcl.inc -nt $o -a $b = h5_game ] || [ -f $O/gen/h5_physavl.inc -a $O/gen/h5_physavl.inc -nt $o -a $b = h5_game ] || [ -n "$(find $O/gen -name 'h5_misc*.inc' -newer $o -print -quit 2>/dev/null)" -a $b = h5_game ] || [ -n "$(find $H/harness2 $H/gles $Z/src -name '*.h' -newer $o -print -quit)" ] || [ -n "$FORCE" ]; then
    ( $CXX $FL -c $f -o $o >$O/log/$b.log 2>&1 && echo "CC $b" ) || { echo "FAIL $b: $(grep -c 'error:' $O/log/$b.log) errores"; rm -f $o; } &
  fi
done; wait
for o in $objs; do [ -f $o ] || { echo "falta $o"; fail=1; }; done
[ $fail = 1 ] && exit 1
SAFEF=$H/harness2/safe0.txt; if [ -n "$H5G" ]; then cat $H/harness2/safe0.txt $H/harness2/safe0_h5.txt ${H5FX:+$H/harness2/safe0_fx.txt} > $O/gen/safe0_all.txt; SAFEF=$O/gen/safe0_all.txt; fi
STUB_LIBS="$LIBS" SAFE0=$SAFEF python3 -I $H/harness/genstubs.py $O/obj $O/stubs.s "$CXX"
$CXX -c $O/stubs.s -o $O/obj/stubs.o
if [ -n "$APP" ]; then
  $CXX -shared -Wl,-u,ANativeActivity_onCreate -Wl,-z,max-page-size=16384 $O/obj/*.o $LIBS -o $O/libmain.so && echo built $O/libmain.so
else
$CXX $O/obj/*.o $LIBS -o $O/p3
echo built $O/p3
fi
