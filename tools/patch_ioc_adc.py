# -*- coding: utf-8 -*-
"""G2026.ioc 补配：ADC1(TIM8_TRGO 触发+DMA2 Stream0 Circular) + 4 按键
改前已有备份 G2026.ioc.bak_20260928_pre_adc
"""
import io, re, sys

P = r"firmware\G2026.ioc"
src = io.open(P, encoding="utf-8").read()
orig = src

# 1) Mcu.IP 列表：加入 ADC1，按字母序重排（CubeMX 要求编号连续 + IPs 总数一致）
ips = re.findall(r"^Mcu\.IP\d+=([^\r\n]+)$", src, re.M)
assert "ADC1" not in ips and len(ips) == 12, ips
ips.append("ADC1")
ips.sort()
new_ip = "".join(f"Mcu.IP{i}={n}\n" for i, n in enumerate(ips)) + "Mcu.IPNb=13\n"
src = re.sub(r"^Mcu\.IP\d+=[^\r\n]*\n(Mcu\.IP\d+=[^\r\n]*\n){11}Mcu\.IPNb=12\n",
             new_ip, src, count=1, flags=re.M)
assert "Mcu.IP0=ADC1" in src and "Mcu.IPNb=13" in src

# 2) PA0 绑定到 ADC1 实例
src = src.replace("PA0-WKUP.Signal=ADCx_IN0", "PA0-WKUP.Signal=ADC1_IN0")
# 半路状态的 Signal-Helper 键删除（ADC1 段接管）
src = src.replace("SH.ADCx_IN0.ConfNb=0\n", "")

# 3) ADC1 参数段 + DMA 段 + NVIC DMA 中断行：插在 NVIC.TimeBaseIP 行后
adc_block = (
    "ADC1.Channel-InputNumber=0\n"
    "ADC1.ClockPrescaler=ADC_CLOCK_SYNC_PCLK_DIV2\n"
    "ADC1.ContinuousConvMode=DISABLE\n"
    "ADC1.DataAlign=ADC_DATAALIGN_RIGHT\n"
    "ADC1.DMAContinuousRequests=ENABLE\n"
    "ADC1.EnableAnalogWatchDog=false\n"
    "ADC1.ExternalTrigConv=ADC_EXTERNALTRIG_T8_TRGO\n"
    "ADC1.ExternalTrigConvEdge=ADC_EXTERNALTRIGCONVEDGE_RISING\n"
    "ADC1.IPParameters=Channel-InputNumber,ClockPrescaler,Resolution,DataAlign,"
    "ScanConvMode,ContinuousConvMode,ExternalTrigConvEdge,ExternalTrigConv,"
    "DMAContinuousRequests,NbrOfConversion,NbrOfConversionFlag,SelectedChannel,SamplingTime\n"
    "ADC1.NbrOfConversion=1\n"
    "ADC1.NbrOfConversionFlag=1\n"
    "ADC1.Resolution=ADC_RESOLUTION_12B\n"
    "ADC1.ScanConvMode=DISABLE\n"
    "ADC1.SelectedChannel=ADC_CHANNEL_0\n"
    "ADC1.SamplingTime=ADC_SAMPLETIME_3CYCLES\n"
)
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
)
nvic_line = "NVIC.DMA2_Stream0_IRQn=true\\:5\\:0\\:false\\:false\\:true\\:false\\:true\\:true\\:true\n"
anchor = "NVIC.TimeBaseIP=TIM6\n"
assert anchor in src
src = src.replace(anchor, anchor + nvic_line + adc_block + dma_block, 1)

# 4) 4 个按键：PE0~PE3 上拉输入（Mcu.Pin 列表 + 引脚参数段）
key_block = ""
for pin, label in [("PE0", "KEY_PAGE"), ("PE1", "KEY_T13"), ("PE2", "KEY_MODE"), ("PE3", "KEY_CAL")]:
    key_block += (
        f"{pin}.GPIOParameters=GPIO_PuPd,GPIO_Label\n"
        f"{pin}.GPIO_PuPd=GPIO_PULLUP\n"
        f"{pin}.GPIO_Label={label}\n"
        f"{pin}.Locked=true\n"
        f"{pin}.Signal=GPIO_Input\n"
    )
anchor2 = "PE7.GPIOParameters=GPIO_Label\n"
assert anchor2 in src
src = src.replace(anchor2, key_block + anchor2, 1)

# Mcu.Pin 索引追加 + 总数更新
pins = re.findall(r"^Mcu\.Pin(\d+)=([^\r\n]+)$", src, re.M)
n = max(int(i) for i, _ in pins)
assert n == 53 and src.count("Mcu.PinsNb=54") == 1
add = "".join(f"Mcu.Pin{54+k}=PE{k}\n" for k in range(4))
src = src.replace("Mcu.PinsNb=54\n", add + "Mcu.PinsNb=58\n", 1)

assert src != orig
io.open(P, "w", encoding="utf-8", newline="\n").write(src)
print("OK: ADC1+DMA2S0+NVIC+4KEY written")
