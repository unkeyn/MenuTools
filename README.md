# MenuTools

**MenuTools** is an ultra-lightweight Windows utility that extends the native Win32 window system menu with essential power-user controls, brings scroll-wheel per-application volume control to the Windows taskbar, and provides an on-demand GPU-accelerated Fullscreen Scaler.

---

## Features

- **Always on Top**: Toggle `WS_EX_TOPMOST` on any window.
- **Hide Top**: Instant borderless windowed mode via `WS_CAPTION` toggle and native `WM_NCCALCSIZE` subclassing (retaining standard resize borders without accent-frame artifacts).
- **Minimize to Tray**: Minimize any application window directly to the system tray notification area.
- **Fullscreen Scaler (`Alt + Shift + A`)**:
  - Standalone, on-demand GPU scaler process (`MenuToolsScaler.exe`) powered by **Windows Graphics Capture (WGC)** and **Direct3D 11 Compute Shaders (`cs_5_0`)**.
  - **Zero Background Overhead**: The scaler process only exists while scaling is active. Focus loss (`Alt + Tab`), window minimization, or target closure immediately terminates the scaler, releasing all GPU and capture resources.
  - **Supported Filters**:
    - `Nearest Neighbor` (ultra-low latency 1 texel load -> 1 texel write)
    - `Pixel Perfect` (true integer scaling >= 1x, or 1:1 center-cropped if oversized, preserving pristine unblurred pixels)
    - `Bicubic` (phase-correct crop-aware 9-tap bilinear Catmull-Rom spline, B=0, C=0.5)
    - `Lanczos` (windowed sinc interpolation)
    - `AMD FidelityFX Super Resolution (FSR 1.0)` (EASU + RCAS passes)
    - `Anime4K 3D` & `Anime4K 3D AA` (official bloc97/Anime4K MIT compute pipeline + 9-tap Catmull-Rom final resize)
  - Features client-area crop, aspect ratio preservation, multi-monitor virtual screen mouse wheel mapping, single-press reactivation, and 32-bit/64-bit cross-architecture support.
- **Taskbar Per-App Volume**:
  - Hover cursor over any application icon on the Windows 11 / 10 taskbar:
    - **Mouse Wheel**: Adjust app volume by ±5%.
    - **Shift + Mouse Wheel**: Fine-tune volume by ±1%.
    - **Middle Click**: Toggle application mute.
  - Minimalist numeric on-screen display (OSD) rendered directly above the taskbar icon.

---

## Карта проекта (Project Map)

```text
MenuTools/
├── MenuCommon/                     # Общие компоненты и IPC протоколы
│   ├── Defines.h                   # Константы, ID фильтров/меню, IPC сообщения и команды
│   ├── TrayIcon.h / TrayIcon.cpp   # Класс работы с системным треем (Shell_NotifyIcon)
│   └── Resource.h                  # Общие строковые ресурсы
│
├── MenuTools/                      # Основной фоновый процесс (менеджер)
│   ├── MenuTools.cpp               # WinMain, регистрация хуков, хоткей Alt+Shift+A, роутер IPC (x86->x64)
│   ├── Hooks.cpp                   # Менеджер внедрения глобальных хуков (WH_CALLWNDPROC, WH_GETMESSAGE)
│   ├── TaskbarVolume.cpp / .h      # Хук мыши WH_MOUSE_LL над таскбаром, CoreAudio (WASAPI) и OSD
│   ├── Startup.cpp / Startup.h     # Настройка автозапуска через планировщик/реестр
│   └── MenuTools.rc                # Иконки и манифест приложения
│
├── MenuToolsHook/                  # Внедряемая DLL (отдельные сборки x86 и x64)
│   ├── MenuTools.cpp               # Инъекция пунктов в системное меню, Hide Top subclassing, статус меню
│   ├── MenuToolsHook.cpp           # Экспортируемые функции хуков CallWndProc и GetMsgProc
│   └── MenuToolsHook64.def         # Таблица экспорта функций для 64-битной DLL
│
└── Scaler/                         # Автономный GPU Fullscreen Scaler
    ├── ScalerMain.cpp              # Точка входа MenuToolsScaler.exe, WGC capture, D3D11 swapchain, WinEvent хуки
    ├── compile_shaders.cpp         # Утилита компиляции шейдеров HLSL в C++ байткод-заголовки
    ├── generate_anime4k.py         # Генератор HLSL шейдеров из спецификаций Anime4K
    └── Shaders/                    # Compute шейдеры HLSL (cs_5_0) и скомпилированные байткоды (.h)
        ├── Nearest_CS.hlsl / .h
        ├── Bicubic_CS.hlsl / .h
        ├── Lanczos_CS.hlsl / .h
        ├── FSR_EASU_CS.hlsl / .h
        ├── FSR_RCAS_CS.hlsl / .h
        └── Anime4K_*.hlsl / .h
```

---

## Архитектура Scaler Lifecycle

```text
Target Window (Foreground)
       │  Alt + Shift + A
       ▼
MenuTools64.exe (Manager)
       │  Spawns with HWND + PID + Filter
       ▼
MenuToolsScaler.exe (Isolated Process)
       ├── Direct3D 11 Device & SwapChain (DXGI_SWAP_EFFECT_FLIP_DISCARD)
       ├── Preallocates Compute Shaders & Intermediate Textures
       ├── Windows Graphics Capture (Direct3D11CaptureFramePool)
       └── Point WinEvent Hooks (EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZESTART)
       │
       ├─► [Alt+Tab / Minimize / Close]
       │         │
       │         ▼
       │   ShutdownScaler()
       │         ├── Close WGC Session & FramePool (Removes yellow capture border)
       │         ├── Release all D3D11 objects
       │         ├── Remove MT_PROP_SCALED & release Local\MenuToolsScaler.Active mutex
       │         └── Scaler process completely terminates
       │
       └─► [User returns to target + Alt+Shift+A]
                 │
                 ▼
           Manager sees !IsScalerActive() -> Launches fresh Scaler on 1st keypress
```

---

## Сборка (Building)

Проект собирается с помощью `llvm-mingw` (`clang++`) со статической линковкой (`-static`), не требуя внешних рантайм-библиотек (`libc++.dll` / `libunwind.dll`).

Для полной автоматизированной сборки всех 5 бинарных файлов (с кодогенерацией Anime4K, компиляцией HLSL, ресурсов и контролем размера):

```cmd
.\build_scaler.cmd
```

Скрипт проверяет строгий лимит размера всех установленных бинарников ($\le 3\text{ MiB}$). Фактический суммарный размер составляет всего **~1.62 MiB** (`MenuToolsScaler.exe` — 273.5 KiB).

---

## Лицензия

Распространяется по лицензии [MIT License](LICENSE).
