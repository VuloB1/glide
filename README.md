# Glide

Limitador de FPS con frame pacing preciso para juegos DX11/DX12 en Windows. Engancha `IDXGISwapChain::Present` y reparte los fotogramas a intervalos exactos para evitar microtirones.

## Características
- Límite de FPS sin deriva (temporizador de alta resolución + espera activa final).
- Selector de procesos, inyección automática al abrir el juego y perfiles por juego.
- Calibración: mide tu rendimiento real y sugiere un límite estable.
- Lista negra de juegos con anti-cheat (EAC, BattlEye, Vanguard, etc.): Glide se niega a inyectarlos.
- Instalador propio, sin dependencias.

## ⚠️ Aviso
Glide inyecta una DLL en el proceso del juego. **Úsalo solo en juegos sin anti-cheat**; en juegos con anti-cheat puede causar un baneo. La lista negra es una ayuda, no una garantía. Software sin garantía, bajo tu responsabilidad.

## Compilar
Requiere Visual Studio 2022 Build Tools y el Windows SDK.

```
build.bat
```

Genera `glide.dll`, `glide-ui.exe` y `glide-setup.exe`.

## Licencia
MIT
