# SimpleFbDxe 180 度旋转

这块屏装反了，需要在 `SimpleFbDxe` 的 Blt 层翻转。

## 1. 声明 PCD

`Silicon/Silicium/SiliciumPkg/SiliciumPkg.dec`，紧跟在 `PcdFrameBufferDelay` 之后：

```
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferRotate|0|UINT32|0x0000A004
```

默认 `0`，不改变其他机型的行为。

## 2. 登记到模块

`Silicon/Silicium/SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf` 的
`[FixedPcd]` 段加上：

```
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferRotate
```

## 3. 平台里打开

`xun.dsc` 的 `[PcdsFixedAtBuild]`：

```
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferRotate|180
```

## 4. 改 Blt

`SimpleFb.c` 里 `FrameBufferGopBlt` 需要：

- 前置声明 `mFrameBuffer`（它定义在函数之后）
- 目标矩形映射到 `(W - x - w, H - y - h)`
- `EfiBltVideoFill` 只需搬矩形，不用翻像素
- `EfiBltBufferToVideo` / `EfiBltVideoToBltBuffer` / `EfiBltVideoToVideo`
  要用临时缓冲把像素按两个轴反向

注意这个 EDK2 版本的 `EFI_GRAPHICS_OUTPUT_BLT_OPERATION` 只有四个值，
没有 `EfiBltBltBufferToBltBuffer`。
