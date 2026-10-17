# -*- coding: utf-8 -*-
"""ADC1 段重建（带 IPParameters 声明）+ IP 列表加 ADC1 —— 不依赖锚点行"""
import io, re

P = r"firmware\G2026.ioc"
s = io.open(P, encoding="utf-8").read()

# 1) ADC1 段重建
s = re.sub(r"ADC1\.[^\r\n]*\n", "", s)
assert "ADC1." not in s
new = (
    "ADC1.ChannelRegularConversion-Channel=ADC_CHANNEL_0\n"
    "ADC1.ChannelRegularConversion-Rank=1\n"
    "ADC1.ChannelRegularConversion-SamplingTime=ADC_SAMPLETIME_3CYCLES\n"
    "ADC1.ClockPrescaler=ADC_CLOCK_SYNC_PCLK_DIV2\n"
    "ADC1.ContinuousConvMode=DISABLE\n"
    "ADC1.DataAlign=ADC_DATAALIGN_RIGHT\n"
    "ADC1.DMAContinuousRequests=ENABLE\n"
    "ADC1.EOCSelection=ADC_EOC_SINGLE_CONV\n"
    "ADC1.ExternalTrigConv=ADC_EXTERNALTRIG_T8_TRGO\n"
    "ADC1.ExternalTrigConvEdge=ADC_EXTERNALTRIGCONVEDGE_RISING\n"
    "ADC1.IPParameters=ChannelRegularConversion-Channel,ChannelRegularConversion-Rank,"
    "ChannelRegularConversion-SamplingTime,ClockPrescaler,Resolution,DataAlign,ScanConvMode,"
    "ContinuousConvMode,EOCSelection,ExternalTrigConvEdge,ExternalTrigConv,DMAContinuousRequests\n"
    "ADC1.Resolution=ADC_RESOLUTION_12B\n"
    "ADC1.ScanConvMode=DISABLE\n"
)
# DMA 段若被规范化删除则重建
if "Dma.Request0=ADC1" not in s:
    new += (
        "Dma.Request0=ADC1\n"
        "Dma.RequestsNb=1\n"
        "Dma.ADC1-Instance=DMA2_Stream0\n"
        "Dma.ADC1-PeriphInc=DMA_PINC_DISABLE\n"
        "Dma.ADC1-MemInc=DMA_MINC_ENABLE\n"
        "Dma.ADC1-PeriphDataAlignment=DMA_PDATAALIGN_HALFWORD\n"
        "Dma.ADC1-MemDataAlignment=DMA_MDATAALIGN_HALFWORD\n"
        "Dma.ADC1-Mode=DMA_CIRCULAR\n"
        "Dma.ADC1-Priority=DMA_PRIORITY_HIGH\n"
    )
# NVIC DMA 行若被删则重建
if "NVIC.DMA2_Stream0_IRQn" not in s:
    new += "NVIC.DMA2_Stream0_IRQn=true\\:5\\:0\\:false\\:false\\:true\\:false\\:true\\:true\\:true\n"
anchor = "Mcu.IPNb="
idx = s.index(anchor)
line_end = s.index("\n", idx) + 1
s = s[:line_end] + new + s[line_end:]

# 2) IP 列表重建：删全部 Mcu.IP 行，按字母序生成含 ADC1 的新列表
ips = re.findall(r"^Mcu\.IP\d+=([^\r\n]+)", s, re.M)
s = re.sub(r"^Mcu\.IP\d+=[^\r\n]*\n", "", s, flags=re.M)
if "ADC1" not in ips:
    ips.append("ADC1")
ips = sorted(set(ips))
block = "".join(f"Mcu.IP{i}={n}\n" for i, n in enumerate(ips))
s = s.replace(f"Mcu.IPNb={len(ips)}\n", "", 1) if f"Mcu.IPNb={len(ips)}\n" in s else s
s = re.sub(r"Mcu\.IPNb=\d+\n", block + f"Mcu.IPNb={len(ips)}\n", s, count=1)
assert "Mcu.IP0=ADC1" in s and f"Mcu.IPNb={len(ips)}" in s
assert "ChannelRegularConversion-Channel" in s and "Dma.Request0=ADC1" in s
io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("OK", len(ips), "IPs:", ",".join(ips))
