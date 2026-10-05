// nfsmw - graphics settings in the menu (F4) that work with the native renderer (see nfsmw_ajustes_graficos.h).

#include "nfsmw_ajustes_graficos.h"

#include "nfsmw_nativo_sistema.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/overlay/settings_overlay.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(nfsmw_resolucion_interna, "automatico", "Graphics",
                      "Resolution the game draws at. Automatic: 1920x1080 docked and 1280x720 handheld, switching on "
                      "the fly when the console is docked or undocked, and also following Reverse-NX. Note: with "
                      "Reverse-NX the clocks do not rise, so 1080p there costs about a third of the FPS. 1280x720 is "
                      "the Xbox 360 resolution. "
                      "1920x1080 always uses 1080p. 1024x576 is the game's own lowest mode: 36 % fewer pixels "
                      "(requires a restart). 640x360 and 640x480 do NOT lower the resolution the scene is drawn "
                      "at (it stays 1280x720), only the size it is shrunk to afterwards: they save a little "
                      "post-processing time, and the upscaling is visible")
    .allowed({"automatico", "1280x720", "1920x1080", "1024x576", "640x360", "640x480"})
    .labels({{"automatico", "Automatic"}})
    .display_name("Internal resolution");

/*
 * GPU MHz in handheld mode.
 *
 * The console starts games at 307.2 MHz. Nintendo later made a 460.8 MHz profile available for
 * the games that need it, and this one does: 25 FPS with the GPU at its limit. This is not an
 * overclock (no KIP or sysmodule is touched): it is requested through the official API (apm), the
 * same one any commercial game uses. Not every game requests it because the ones that do not
 * need it get longer battery life.
 *
 * 307.2 -> 460.8 is +50 % GPU. It costs battery and heat; with 0 the console stays
 * exactly as it was.
 *
 * How to check that it worked: the profiler reports the real frequency in every report
 * ("relojes: CPU 1020.0 MHz, GPU 307.2 MHz, ..."). If it still says 307.2, it did not take effect.
 */
REXCVAR_DEFINE_INT32(nfsmw_switch_gpu_mhz, 460, "Graphics",
                     "Switch: GPU clock in MHz requested from the system in handheld mode (0 = leave it "
                     "alone, 384, 460). 460.8 MHz is an official Horizon profile, not an overclock; it uses "
                     "more battery and runs warmer")
    .allowed({"0", "384", "460"})
    .display_name("Handheld GPU clock (MHz)");

/*
 * The trade-off, just in case.
 *
 * The high GPU configurations come in two flavors: memory at 1331.2 (0x92220008) and memory at
 * 1600 (0x92220007). We want the first, because raising the RAM clock costs battery and heat
 * without giving anything back: our measurements put bandwidth at 27 %, and what saturates is the
 * fragment ALU. If this firmware lacks it, the default is to leave the GPU as it was rather than
 * raise the RAM clock. With this set to true the trade-off is accepted: high GPU with RAM at 1600.
 */
REXCVAR_DEFINE_BOOL(nfsmw_switch_ram_1600, false, "Graphics",
                    "Switch: accept the high GPU profile even if it raises the memory clock to 1600 MHz. "
                    "Off by default: if no profile keeps the RAM where it was, the GPU is left as it was. "
                    "The [apm] log line says which one was applied")
    .display_name("Allow RAM 1600 MHz profile");

REXCVAR_DEFINE_BOOL(nfsmw_switch_saltynx, true, "Graphics",
                    "Switch: publishes the FPS and resolution where the console overlays (SaltyNX) read "
                    "them, and follows Reverse-NX's handheld/docked mode. Does nothing without SaltyNX")
    .display_name("SaltyNX overlay info");

REXCVAR_DEFINE_BOOL(nfsmw_switch_relojes_reverse, false, "Graphics",
                    "Switch: make the clocks follow Reverse-NX when it does not match the hardware, in BOTH "
                    "directions, by asking the sysmodule (Horizon OC and other sys-clk forks). If it fakes DOCKED "
                    "while handheld, YOUR docked column is applied (the game profile's or the global one): that "
                    "is an overclock and not valid for measuring. If it fakes HANDHELD while docked, the clocks "
                    "drop to the handheld column or, if that is empty, to the stock clocks (GPU 307.2 MHz, memory "
                    "1331.2). Released when they match again and on exit")
    .display_name("Clocks follow Reverse-NX");
/*
 * The rate it really ticks at, measured on the console (race, no overclock).
 *
 * nfsmw_limite_fps ends up in video_mode_refresh_rate, and BucleVblank (nfsmw_nativo_sistema.cpp) reads
 * it from there to know how often to fire the guest interrupt. With "60" the vblank thread ticks at
 * exactly 60.1 Hz: 601 vblanks per 10 s of log, spot on in every interval.
 *
 * It neither throttles nor drops work, and both are measured, not assumed:
 *   - It does not throttle: the game runs at 24.5 swaps/s, far below 60. The vblanks/swaps ratio is
 *     2.45, which is not an integer; if the game limited itself by counting vblanks it would be 2.00
 *     or 3.00. The pace is set by the work, not by the vblank.
 *   - Nothing is dropped: "presentados=6260 rechazos=0" in the log, and the profiler checks that the
 *     game presents 25.35/s and 25.35/s reach the window (nwindowQueueBuffer), deviation +0.0 %. Not
 *     a single frame is lost in the output mailbox.
 * Conclusion: at 60 there is nothing to gain or lose here. Setting "30" would only lower the guest
 * interrupt to 30 Hz (0.25 % of a core) and tell the game that the panel runs at 30; as long as a
 * frame takes 39 ms nothing changes. The time missing to reach 33.3 ms is not in this setting: it is
 * in the PM4 ring thread spending 6.4-7.5 ms per frame inside presentation without recording the next
 * frame.
 */
REXCVAR_DEFINE_STRING(nfsmw_limite_fps, "60", "Graphics",
                      "Maximum game FPS. 60: no limit of its own (the game follows a 60 Hz vblank). 30: a steady "
                      "30 FPS pace without ups and downs, with the game at normal speed. Applies after a restart")
    .allowed({"60", "30"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart)
    .display_name("FPS limit");

/*
 * Pace fixed by hardware, not by software.
 *
 * nfsmw_limite_fps does not limit presentation: it tells the native renderer's vblank thread at
 * what rate to fire the guest interrupt, and the game limits itself by counting vblanks. Those are two
 * different free-running 30 Hz clocks (the process timer and the panel), and they beat against each
 * other: most frames at 33.3 ms and every few seconds one at 50. Micro-stutters with the counter
 * showing 30.
 *
 * This is the other approach: it asks the compositor for one image every N vblanks. With 2 there is a
 * single clock, the panel's, and its cadence propagates back through backpressure to the PM4 ring
 * thread. The WSI sets it to 1 when the swapchain is created, so it is reapplied on every presentation
 * (switch_perf.cpp).
 */
/*
 * The default is 0, and this matters.
 *
 * The console swapchain is created in IMMEDIATE mode, which in the Horizon WSI means
 * nwindowSetSwapInterval(nw, 0): the queue stops blocking the producer. That is what closes the
 * 6.3-7.8 ms gap in which the GPU sits idle while the PM4 ring thread is inside presentation
 * (measured on the console: frame = GPU work + gap, exactly).
 *
 * But AnotarRitmo reapplies the interval on every presentation (on purpose, because the swapchain is
 * recreated when switching from docked to handheld mode). With the default at 2, or with a 1 in the
 * toml, the WSI's 0 was overwritten on the first frame and IMMEDIATE did nothing. With 0 it is left
 * alone and the swapchain mode applies, which is what we want.
 *
 * If the panel ever has to be locked again: 2 still works, but it only makes sense when the work
 * really fits in 33.3 ms. Because of the gap it does not fit, and with the gap closed interval 0
 * gives the same result without the risk of dropping to the next step.
 */
REXCVAR_DEFINE_UINT32(nfsmw_intervalo_swap, 0, "Graphics",
                      "Switch: vblanks between frames. 0 = leave it to the swapchain mode (the normal setting: "
                      "IMMEDIATE, i.e. interval 0, which removes the idle gap). 2 = 30 FPS locked by the panel, "
                      "only useful if the work fits in 33.3 ms. 1 = up to 60")
    .display_name("Swap interval");

// Optional antialiasing on the output, applied live. The Xbox 360 draws the scene with 4x MSAA; the Switch
// has none (it costs GPU time and hung NVK in another port), and FXAA smooths the edges with little work.
REXCVAR_DEFINE_STRING(nfsmw_antialiasing, "apagado", "Graphics",
                      "Image antialiasing. Off: no antialiasing. FXAA: smooths jagged edges (the Xbox 360 uses 4x "
                      "MSAA, which is not available here); costs some GPU time")
    .allowed({"apagado", "fxaa"})
    .labels({{"apagado", "Off"}, {"fxaa", "FXAA"}})
    .display_name("Antialiasing");

// Sky glow, applied live. The game's bright pass (PS n137) subtracts the threshold per channel: with an
// intense blue sky only the blue channel passes and leaves blue edges over the trees. natural: the same
// glow energy with the hue of the source color. suave: the threshold is applied to luminance, with much
// less halo.
// natural is the default: compared on the console, original still showed a noticeable blue halo.
REXCVAR_DEFINE_STRING(nfsmw_resplandor_cielo, "natural", "Graphics",
                      "Sky glow. Natural (default): the game's glow without the saturated blue that leaves edges on "
                      "trees and rooftops; colored lights look the same. Original: as on the Xbox 360, with those "
                      "blue edges. Soft: almost no sky halo; single-color lights (brake lights, police) glow less. "
                      "No performance cost")
    .allowed({"original", "natural", "suave"})
    .labels({{"original", "Original"}, {"natural", "Natural"}, {"suave", "Soft"}})
    .display_name("Sky glow");

// The game's color filter, applied live: its "visual treatment" (the yellow tint, the desaturation and the
// vignette of the final composite). Some players find the tint too strong in some areas of the city.
REXCVAR_DEFINE_STRING(nfsmw_tratamiento_visual, "original", "Graphics",
                      "The game's color filter (the yellow tint, the desaturation and the vignette). Original "
                      "(default): as on the Xbox 360. Soft: half strength. Off: no filter; the glow and the fades "
                      "stay the same. No performance cost")
    .allowed({"original", "suave", "apagado"})
    .labels({{"original", "Original"}, {"suave", "Soft"}, {"apagado", "Off"}})
    .display_name("Color filter");

// Optional post-processing, applied live (visible when changed with the menu open). Option lists for the
// gamepad. The presets use the values from GoldenEye-Recomp (ge_postfx.cpp, public domain).
REXCVAR_DEFINE_STRING(nfsmw_posproceso, "apagado", "Graphics/Post-processing",
                      "Image post-processing. Off: as on the Xbox 360. Presets: Cinema, Sepia, Noir, Cool, Warm, "
                      "Vivid, Matrix and CRT. Custom: the values below. Single-channel grading is free; saturation, "
                      "vibrance, vignette and scanlines do some work per pixel")
    .allowed({"apagado", "cine", "sepia", "noir", "frio", "calido", "vivo", "matrix", "crt", "personalizado"})
    .labels({{"apagado", "Off"},
             {"cine", "Cinema"},
             {"sepia", "Sepia"},
             {"noir", "Noir"},
             {"frio", "Cool"},
             {"calido", "Warm"},
             {"vivo", "Vivid"},
             {"matrix", "Matrix"},
             {"crt", "CRT"},
             {"personalizado", "Custom"}})
    .display_name("Post-processing");
REXCVAR_DEFINE_STRING(nfsmw_posproceso_brillo, "0.00", "Graphics/Post-processing",
                      "Custom: brightness added (0.00 = no change)")
    .allowed({"-0.20", "-0.15", "-0.10", "-0.05", "0.00", "+0.05", "+0.10", "+0.15", "+0.20"})
    .display_name("Brightness");
REXCVAR_DEFINE_STRING(nfsmw_posproceso_contraste, "1.00", "Graphics/Post-processing",
                      "Custom: contrast around mid-gray (1.00 = no change)")
    .allowed({"0.70", "0.80", "0.90", "1.00", "1.10", "1.20", "1.30", "1.40"})
    .display_name("Contrast");
REXCVAR_DEFINE_STRING(nfsmw_posproceso_saturacion, "1.00", "Graphics/Post-processing",
                      "Custom: saturation (0.00 = black and white, 1.00 = no change)")
    .allowed({"0.00", "0.25", "0.50", "0.75", "1.00", "1.10", "1.20", "1.35", "1.50", "2.00"})
    .display_name("Saturation");
REXCVAR_DEFINE_STRING(nfsmw_posproceso_vibracion, "0.00", "Graphics/Post-processing",
                      "Custom: vibrance, boosts less saturated colors more (0.00 = no change)")
    .allowed({"-0.50", "-0.25", "0.00", "+0.15", "+0.25", "+0.50", "+0.75", "+1.00"})
    .display_name("Vibrance");
REXCVAR_DEFINE_STRING(nfsmw_posproceso_temperatura, "0.00", "Graphics/Post-processing",
                      "Custom: color temperature, warmer (+) or cooler (-) (0.00 = no change)")
    .allowed({"-1.00", "-0.75", "-0.50", "-0.25", "0.00", "+0.25", "+0.50", "+0.75", "+1.00"})
    .display_name("Temperature");
REXCVAR_DEFINE_STRING(nfsmw_posproceso_gamma, "1.00", "Graphics/Post-processing",
                      "Custom: gamma, above 1 brightens the midtones (1.00 = no change)")
    .allowed({"0.70", "0.80", "0.90", "1.00", "1.10", "1.20", "1.30"})
    .display_name("Gamma");
REXCVAR_DEFINE_STRING(nfsmw_posproceso_vineta, "0.00", "Graphics/Post-processing",
                      "Custom: vignette, darkens the edges (0.00 = no vignette)")
    .allowed({"0.00", "0.20", "0.30", "0.40", "0.55", "0.70", "1.00"})
    .display_name("Vignette");
REXCVAR_DEFINE_STRING(nfsmw_posproceso_lineas, "0.00", "Graphics/Post-processing",
                      "Custom: old TV scanlines, one row in three (0.00 = no scanlines)")
    .allowed({"0.00", "0.25", "0.50", "0.75", "1.00"})
    .display_name("Scanlines");

namespace nfsmw::ajustes {
namespace {

std::atomic<uint64_t> g_version_posproceso{1};
std::atomic<bool> g_fxaa{false};
std::atomic<int> g_resplandor_cielo{0};  // 0 original, 1 natural, 2 soft
std::atomic<int> g_tratamiento_visual{0};  // 0 original, 1 soft, 2 off

constexpr const char* kCvarsPosproceso[] = {
    "nfsmw_posproceso",           "nfsmw_posproceso_brillo",      "nfsmw_posproceso_contraste",
    "nfsmw_posproceso_saturacion", "nfsmw_posproceso_vibracion",  "nfsmw_posproceso_temperatura",
    "nfsmw_posproceso_gamma",     "nfsmw_posproceso_vineta",      "nfsmw_posproceso_lineas",
};

// nfsmw_resplandor_cielo: 0 original, 1 natural, 2 suave.
int ModoResplandor(std::string_view valor) {
  return valor == "natural" ? 1 : valor == "suave" ? 2 : 0;
}

// nfsmw_tratamiento_visual: 0 original, 1 suave, 2 apagado.
int ModoTratamiento(std::string_view valor) {
  return valor == "suave" ? 1 : valor == "apagado" ? 2 : 0;
}

float Numero(const char* nombre, float por_defecto) {
  const std::string texto = rex::cvar::GetFlagByName(nombre);
  char* fin = nullptr;
  const double valor = std::strtod(texto.c_str(), &fin);
  return fin && fin != texto.c_str() ? float(valor) : por_defecto;
}

// The command line takes precedence (PC tests with --video_mode_width=...). Whatever comes from the
// configuration file is replaced: SaveConfig saves the cvars that differ from their default value, and an
// old video mode must not win over the new settings.
void Poner(const char* nombre, const std::string& valor) {
  if (rex::cvar::GetFlagInfo(nombre) == nullptr) {
    REXLOG_WARN("[ajustes] el cvar {} no existe: no se aplica {}", nombre, valor);
    return;
  }
  if (rex::cvar::GetFlagSource(nombre) == rex::cvar::Source::kCommandLine) {
    REXLOG_INFO("[ajustes] {} viene de la linea de comandos ({}): no se toca", nombre,
                rex::cvar::GetFlagByName(nombre));
    return;
  }
  if (!rex::cvar::SetFlagByName(nombre, valor)) {
    REXLOG_WARN("[ajustes] no se pudo poner {} = {}", nombre, valor);
  }
}

}  // namespace

#if REX_PLATFORM_SWITCH
// Provided by the SaltyNX module so that the console overlays can show the resolution.
extern "C" void RexSwitchPerfResolution(unsigned ancho, unsigned alto);
extern "C" void RexSwitchPerfIntervaloSwap(unsigned vblanks);
extern "C" void RexSwitchSaltyNxHabilitar(int habilitado);
extern "C" void RexSwitchRelojesReverseHabilitar(int habilitado);
extern "C" void RexSwitchApmPedirGpuMhz(int mhz);
extern "C" void RexSwitchApmPermitirRam1600(int permitir);
extern "C" void RexSwitchApmAplicar(void);
#endif

void AplicarAjustesGraficos() {
  const std::string resolucion = REXCVAR_GET(nfsmw_resolucion_interna);
  // 1920x1080 is not requested through the video mode. The game has its own table and above 1280x720 it
  // stays at 1280x720, so asking for it does nothing. What does work is writing the size into the mode
  // table of its renderer: nfsmw_render_targets.cpp does that, reading this same cvar, and also switches
  // it on the fly when the console enters or leaves the dock (automatic mode). The video mode stays at
  // 720p, which is what is presented. Setting the video mode to 1920x1080 so that the game's front buffer
  // would grow with it was tried and does not work: the front buffer is registered earlier and stays at
  // 1280x720.
  const bool interno_1080p = resolucion == "1920x1080" || resolucion == "automatico";
  const std::string modo_video = interno_1080p ? std::string("1280x720") : resolucion;
  const size_t x = modo_video.find('x');
  if (x != std::string::npos) {
    Poner("video_mode_width", modo_video.substr(0, x));
    Poner("video_mode_height", modo_video.substr(x + 1));
  }
#if REX_PLATFORM_SWITCH
  // FPS and resolution for the console overlays, and honoring Reverse-NX. Can be turned off if it gets in the way.
  RexSwitchSaltyNxHabilitar(REXCVAR_GET(nfsmw_switch_saltynx) ? 1 : 0);
  RexSwitchRelojesReverseHabilitar(REXCVAR_GET(nfsmw_switch_relojes_reverse) ? 1 : 0);
  // GPU profile in handheld mode. See nfsmw_switch_gpu_mhz.
  RexSwitchApmPedirGpuMhz(REXCVAR_GET(nfsmw_switch_gpu_mhz));
  RexSwitchApmPermitirRam1600(REXCVAR_GET(nfsmw_switch_ram_1600) ? 1 : 0);
  RexSwitchApmAplicar();
  RexSwitchPerfIntervaloSwap(unsigned(REXCVAR_GET(nfsmw_intervalo_swap)));
#endif
  Poner("resolution", "");  // empty preset: the width and height above apply
  const std::string limite = REXCVAR_GET(nfsmw_limite_fps);
  Poner("video_mode_refresh_rate", limite == "30" ? "30" : "60");
  REXLOG_INFO("[ajustes] resolucion interna {} y limite de FPS {}: modo de video {}x{} a {} Hz", resolucion, limite,
              rex::cvar::GetFlagByName("video_mode_width"), rex::cvar::GetFlagByName("video_mode_height"),
              rex::cvar::GetFlagByName("video_mode_refresh_rate"));
#if REX_PLATFORM_SWITCH
  {
    // The overlay shows the resolution the game draws at, so with the 1080p internal mode it has to be
    // told 1920x1088, which is the real scene render target, not the video mode.
    unsigned ancho = unsigned(std::strtoul(rex::cvar::GetFlagByName("video_mode_width").c_str(), nullptr, 10));
    unsigned alto = unsigned(std::strtoul(rex::cvar::GetFlagByName("video_mode_height").c_str(), nullptr, 10));
    // nfsmw_render_targets.cpp publishes the real size every time the mode changes, because with
    // "automatico" it changes on the fly. Only the startup value is set here.
    if (resolucion == "1920x1080") {
      ancho = 1920;
      alto = 1080;
    }
    if (ancho && alto) {
      RexSwitchPerfResolution(ancho, alto);  // for the console overlay
    }
  }
#endif
}

void OcultarAjustesSinEfecto() {
  if (!nfsmw::nativo::Activo()) {
    return;  // with emulation, all of them do something
  }
  const std::vector<std::string> ocultos = {
      // From the emulated path: they do nothing with the native renderer.
      "resolution_scale", "draw_resolution_scale_x", "draw_resolution_scale_y", "vsync", "anisotropic_override",
      "swap_post_effect",
      // Replaced by nfsmw_resolucion_interna and nfsmw_limite_fps.
      "video_mode_width", "video_mode_height", "resolution", "video_mode_refresh_rate",
  };
  // On PC, the emulated path settings are registered by the GPU plugin, which is not loaded with the
  // native renderer: they do not exist there. On the Switch they are built into the executable and do show
  // up in the menu. They are hidden by name in both cases.
  std::string registrados;
  std::string sin_registrar;
  for (const std::string& nombre : ocultos) {
    std::string& lista = rex::cvar::GetFlagInfo(nombre) ? registrados : sin_registrar;
    lista += (lista.empty() ? "" : ", ") + nombre;
  }
  rex::ui::OcultarAjustesEnMenu(ocultos);
  REXLOG_INFO("[ajustes] menu: fuera, porque no hacen nada con el renderizador nativo o los sustituye la categoria "
              "Graphics: {}; no registrados en esta build: {}",
              registrados.empty() ? "ninguno" : registrados, sin_registrar.empty() ? "ninguno" : sin_registrar);
}

Posproceso LeerPosproceso() {
  struct Preajuste {
    const char* nombre;
    Posproceso valores;
  };
  // The GoldenEye-Recomp ones (ge_postfx.cpp): brightness, contrast, saturation, vibrance, temperature,
  // gamma, tint (red, green, blue and strength), vignette and scanlines.
  static const Preajuste kPreajustes[] = {
      {"cine", {true, -0.04f, 1.15f, 1.05f, 0.15f, -0.10f, 1.0f, 1.0f, 0.95f, 0.85f, 0.12f, 0.40f, 0.0f}},
      {"sepia", {true, -0.02f, 1.05f, 0.20f, 0.0f, 0.25f, 1.0f, 1.0f, 0.82f, 0.55f, 0.50f, 0.30f, 0.0f}},
      {"noir", {true, -0.03f, 1.35f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.55f, 0.0f}},
      {"frio", {true, 0.0f, 1.05f, 1.0f, 0.10f, -0.55f, 1.0f, 0.70f, 0.85f, 1.0f, 0.15f, 0.18f, 0.0f}},
      {"calido", {true, 0.02f, 1.05f, 1.05f, 0.15f, 0.55f, 1.0f, 1.0f, 0.85f, 0.60f, 0.12f, 0.18f, 0.0f}},
      {"vivo", {true, 0.0f, 1.10f, 1.20f, 0.50f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f}},
      {"matrix", {true, -0.03f, 1.10f, 0.85f, 0.20f, 0.0f, 1.0f, 0.55f, 1.0f, 0.60f, 0.30f, 0.30f, 0.25f}},
      {"crt", {true, -0.02f, 1.05f, 1.10f, 0.10f, 0.0f, 1.0f, 0.90f, 1.0f, 0.90f, 0.10f, 0.20f, 0.50f}},
  };
  const std::string modo = rex::cvar::GetFlagByName("nfsmw_posproceso");
  for (const Preajuste& p : kPreajustes) {
    if (modo == p.nombre) {
      return p.valores;
    }
  }
  Posproceso resultado;
  if (modo != "personalizado") {
    return resultado;  // apagado
  }
  resultado.activo = true;
  resultado.brillo = Numero("nfsmw_posproceso_brillo", 0.0f);
  resultado.contraste = Numero("nfsmw_posproceso_contraste", 1.0f);
  resultado.saturacion = Numero("nfsmw_posproceso_saturacion", 1.0f);
  resultado.vibracion = Numero("nfsmw_posproceso_vibracion", 0.0f);
  resultado.temperatura = Numero("nfsmw_posproceso_temperatura", 0.0f);
  resultado.gamma = Numero("nfsmw_posproceso_gamma", 1.0f);
  resultado.vineta = Numero("nfsmw_posproceso_vineta", 0.0f);
  resultado.lineas = Numero("nfsmw_posproceso_lineas", 0.0f);
  return resultado;
}

uint64_t VersionPosproceso() {
  return g_version_posproceso.load(std::memory_order_relaxed);
}

bool AntialiasingFxaa() {
  return g_fxaa.load(std::memory_order_relaxed);
}

int ResplandorCielo() {
  return g_resplandor_cielo.load(std::memory_order_relaxed);
}

int TratamientoVisual() {
  return g_tratamiento_visual.load(std::memory_order_relaxed);
}

void VigilarAjustesEnVivo() {
  // The notification arrives with the registry lock held (SetFlagFromSource): only atomics are touched.
  for (const char* nombre : kCvarsPosproceso) {
    rex::cvar::RegisterChangeCallback(nombre, [](std::string_view, std::string_view) {
      g_version_posproceso.fetch_add(1, std::memory_order_relaxed);
    });
  }
  g_fxaa.store(rex::cvar::GetFlagByName("nfsmw_antialiasing") == "fxaa", std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("nfsmw_antialiasing", [](std::string_view, std::string_view valor) {
    g_fxaa.store(valor == "fxaa", std::memory_order_relaxed);
  });
  g_resplandor_cielo.store(ModoResplandor(rex::cvar::GetFlagByName("nfsmw_resplandor_cielo")),
                          std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("nfsmw_resplandor_cielo", [](std::string_view, std::string_view valor) {
    g_resplandor_cielo.store(ModoResplandor(valor), std::memory_order_relaxed);
  });
  g_tratamiento_visual.store(ModoTratamiento(rex::cvar::GetFlagByName("nfsmw_tratamiento_visual")),
                             std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("nfsmw_tratamiento_visual", [](std::string_view, std::string_view valor) {
    g_tratamiento_visual.store(ModoTratamiento(valor), std::memory_order_relaxed);
  });
}

}  // namespace nfsmw::ajustes
