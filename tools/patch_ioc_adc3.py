# -*- coding: utf-8 -*-
"""补回 ADC1 触发/DMA 键（ADC1 实例已由 GUI 激活，平铺键名有效）+ Dma 段 + NVIC"""
import io

P = r"C:\Users\funiang\Desktop\Project\projects\G2026\firmware\G2026.ioc"
s = io.open(P, encoding="utf-8").read()

# 1) IPParameters 追加三个触发键声明
old_ip = ("ADC1.IPParameters=Rank-0\\#ChannelRegularConversion,master,"
          "Channel-0\\#ChannelRegularConversion,SamplingTime-0\\#ChannelRegularConversion,"
          "NbrOfConversionFlag")
new_ip = old_ip + ",ExternalTrigConv,ExternalTrigConvEdge,DMAContinuousRequests"
assert old_ip in s, "IPParameters anchor not found"
s = s.replace(old_ip, new_ip, 1)

# 2) 触发/DMA 请求键（挂在 ADC1.master 行后）
anchor = "ADC1.master=1\n"
add = (
    "ADC1.ExternalTrigConv=ADC_EXTERNALTRIG_T8_TRGO\n"
    "ADC1.ExternalTrigConvEdge=ADC_EXTERNALTRIGCONVEDGE_RISING\n"
    "ADC1.DMAContinuousRequests=ENABLE\n"
)
assert anchor in s
s = s.replace(anchor, anchor + add, 1)

# 3) Dma 段 + NVIC DMA 中断行（锚在 Mcu.IPNb= 行后）
dma_block = (
    "Dma.Request0=ADC1\n"
    "Dma.RequestsNb=1\n"
    "Dma.ADC1-Instance=DMA2_Stream0\n"
    "Dma.ADC1-PeriphInc=DMA_PINC_DISABLE\n"
    "Dma.ADC1-MemInc=DMA_MINC_ENABLE\n"
    "Dma.ADC1-PeriphDataAlignment=DMA_PDATAALIGN_HALFWORD\n"
    "Dma.ADC1-MemDataAlignment=DMA_MDATAALIGN_HALFWORD\n"
    "Dma.ADC1-Mode=DMA_CIRCULAR\n"
    "Dma.ADC1-Priority=DMA_PRIORITY_HIGH\n"
    "NVIC.DMA2_Stream0_IRQn=true\\:5\\:0\\:false\\:false\\:true\\:false\\:true\\:true\\:true\n"
)
idx = s.index("Mcu.IPNb=")
line_end = s.index("\n", idx) + 1
s = s[:line_end] + dma_block + s[line_end:]

io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("OK: trigger keys + DMA section + NVIC written")
