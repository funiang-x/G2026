# -*- coding: utf-8 -*-
"""回滚 .ioc 里的 ADC1（尊重既有 bsp_adc.c 直写寄存器的设计），保留按键 + NVIC.DMA2 行"""
import io, re

P = r"C:\Users\funiang\Desktop\Project\projects\G2026\firmware\G2026.ioc"
s = io.open(P, encoding="utf-8").read()

# 1) 删全部 ADC1.* 键
s = re.sub(r"ADC1\.[^\r\n]*\n", "", s)
# 2) 删全部 Dma.* 键（DMA 由 bsp_adc.c 自己 init）
s = re.sub(r"Dma\.[^\r\n]*\n", "", s)

# 3) IP 列表去 ADC1（回 12 个）
ips = re.findall(r"^Mcu\.IP\d+=([^\r\n]+)", s, re.M)
s = re.sub(r"^Mcu\.IP\d+=[^\r\n]*\n", "", s, flags=re.M)
ips = sorted(set(i for i in ips if i != "ADC1"))
block = "".join(f"Mcu.IP{i}={n}\n" for i, n in enumerate(ips))
s = re.sub(r"Mcu\.IPNb=\d+\n", block + f"Mcu.IPNb={len(ips)}\n", s, count=1)

# 4) PA0 恢复基线：Signal=ADCx_IN0 + SH.ConfNb=0（绑定行删除）
s = s.replace("SH.ADCx_IN0.0=ADC1_IN0,IN0\n", "")
s = s.replace("SH.ADCx_IN0.ConfNb=1\n", "SH.ADCx_IN0.ConfNb=0\n")
s = s.replace("PA0-WKUP.Mode=IN0\n", "")

# 5) 确保 NVIC.DMA2_Stream0_IRQn 行存在（决定 it.c 是否生成 handler —— bsp_adc 依赖）
if "NVIC.DMA2_Stream0_IRQn" not in s:
    anchor = "NVIC.TimeBaseIP=TIM6\n"
    assert anchor in s
    s = s.replace(anchor,
        anchor + "NVIC.DMA2_Stream0_IRQn=true\\:5\\:0\\:false\\:false\\:true\\:false\\:true\\:true\\:true\n", 1)

assert "ADC1." not in s and "Dma." not in s
assert "Mcu.IPNb=12" in s and "KEY_PAGE" in s and "NVIC.DMA2_Stream0_IRQn" in s
assert "PE0.Signal=GPIO_Input" in s
io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("rollback OK:", len(ips), "IPs")
