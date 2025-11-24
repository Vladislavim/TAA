<p align="center">
  <img src="https://github.com/Vladislavim/TAA/blob/master/photo_2025-11-22_02-23-52.jpg" alt="TAA + Pseudo RT + Procedural Fog demo" width="900">
</p>

# TAA + Pseudo RT Shadows + Procedural Fog (D3D12)

Учебный проект на базе **SsaoApp** из книги Frank Luna  
(DirectX 12, пример с SSAO и shadow map), переработанный под:

- **TAA (Temporal Anti-Aliasing)** с Halton-джиттером и history-buffer’ом  
- **Псевдо ray tracing-тени** (смягчённые тени по глубине)  
- **Процедурный height-fog**, меняющийся от *dirty* к *clean* при `AtmosphereCleanliness ∈ [0,1]`

---

## Основные фичи

### TAA

- Halton 2/3 джиттер проекции в сабпиксельном диапазоне  
- History A/B ping-pong того же формата, что back-buffer  
- Reprojection через `InvViewProj` + `PrevViewProj`  
- Depth rejection и neighborhood-clamping по 3×3 окну, чтобы не мазало контуры  
- Режимы просмотра (клавиша **Y**):
  - 0 – итоговый TAA  
  - 1 – текущий кадр  
  - 2 – history (после clamping)  
  - 3 – `abs(curr - history)`  
  - 4 – отладка области черепа (stencil)

### Псевдо RT-тени

- Базовый **Luna PCF** + режим мягких теней:
  - радиус выборки зависит от расстояния,  
  - визуально ближе к area-light / RT-теням  
- Переключение режимов — клавиша **H**

### Процедурный fog

- Exponential height-fog: плотность зависит от высоты и дистанции  
- Цвет тумана плавно меняется по высоте (земля → небо)  
- Параметр `AtmosphereCleanliness`:
  - `0` — грязный, плотный смог  
  - `1` — чистый воздух, лёгкий голубой haze  
- Общая яркость контролируется `AtmosphereIntensity`

---

## Управление

- **W / A / S / D** – движение камеры  
- **ЛКМ + мышь** – вращение  
- **T** – TAA ON/OFF  
- **Y** – режимы TAA  
- **U / J** – `TaaFeedback`  
- **I / K** – амплитуда джиттера  
- **H** – режим теней (PCF / soft)  
- **Z / X** – `AtmosphereCleanliness` (dirty ⇄ clean)  
- **G** – wireframe  

Текущие значения TAA / fog / теней выводятся в заголовке окна.
