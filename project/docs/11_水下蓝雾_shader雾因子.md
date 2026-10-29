# 11 水下蓝雾：把 NV2A 雾因子传给像素着色器（1.1.8-t）

## 现象

第十一章水下抬头看水面时，主机/对比图有整屏蓝绿色水下色调，PC 版没有；水上水面正常。
1.1.7-t 的尝试（只保护 `256×256` 水面缓冲）没有解决这一项。

## 定位

- `effectsnative = 1`：无变化 → 不是小缓冲尺寸；
- `cfg:vsdivide:0`：无变化 → 不是折射坐标；
- 强制 `CULLMODE = NONE`：无变化 → 不是背面剔除；
- 反向线性雾打开/关闭：无变化；
- 抓水面帧的 draw 状态：雾色 `0xff315155`，水下场景中大量像素着色器 draw 的 `FOGENABLE` 为真；
- 旧规则 `!g_st.pixel_shader` 让所有像素着色器 draw 都不吃 host 雾，所以蓝雾消失。

## 根因

不是“水面 shader 没画”，而是 NV2A 的 FOG 寄存器只给了雾色，没有给雾因子：

```hlsl
float4 FOGR = float4(fogColor.rgb, 1);   // 旧：alpha 固定 1
```

xemu 的实现是：

```c
vec4 pFog = vec4(fogColor.rgb, clamp(vtxFog, 0.0, 1.0));
```

即 FOG 寄存器的 alpha 是**逐像素雾因子**。旧代码把 alpha 固定成 1，只能靠 host 雾模拟；一旦水 shader 自己读 FOG，host 雾开关就会在“蓝雾消失”和“水面发黑”之间二选一。

## 修法（1.1.8-t）

1. `nv2a_vsh.cpp`：顶点着色器把 `oFog` 按雾表算成因子，经 `TEXCOORD4` 插值到像素端：

```hlsl
float4 t4 : TEXCOORD4;
...
o.t4 = float4(o.fog, 0, 0, 1);
```

2. `nv2a_psh.cpp`：像素着色器读取该因子，写进 FOG 寄存器：

```hlsl
float4 FOGR = float4(fogColor.rgb, saturate(I.tc4.x));
```

3. `draw.cpp`：host 雾只保留给固定管线：

```cpp
fog = FOGENABLE && !no_fog && !pixel_shader;
```

像素着色器不再叠 D3D9 post-shader 雾，避免双重雾。

## 验证

- 用户实机（Intel 核显）第十一章水下：蓝绿色恢复，水面不再发黑；
- 人体/影子：未再出现雾染色；
- 第 3 章天窗反向范围仍按原规则关闭，未复现整屏变棕；
- 用户对照主机截图确认色调一致。

## 已知未修

同一光柱/龙头位置，轻微移动视角时原有 glow/光晕仍会抖。1.1.7-t 基线同样存在，与本次 shader 雾无关；不纳入 1.1.8-t。

## 风险

- `TEXCOORD4` 是当前 NV2A VSOut 未使用的插值通道；以后若游戏 shader 用到 TEXCOORD4，需要换通道或改为打包；
- 目前只验证了水下蓝雾和影子；21:9、Boss、其他雾色场景仍需抽测；
- 这是逐像素雾因子的第一版，不是完整重写 NV2A 雾单元。
