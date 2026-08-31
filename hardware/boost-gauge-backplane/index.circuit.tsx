import { LMR36520ADDAR } from "./imports/LMR36520ADDAR"
import { TCA9406DCTR } from "./imports/TCA9406DCTR"
import { ADS1115IDGSR } from "./imports/ADS1115IDGSR"
import { BMP280 } from "./imports/BMP280"
import { DS3231MZ_TRL } from "./imports/DS3231MZ_TRL"
import { DMP4065S_7 } from "./imports/DMP4065S_7"
import { SMBJ16CA } from "./imports/SMBJ16CA"
import { SS54 } from "./imports/SS54"
import { FXL0530_100_M } from "./imports/FXL0530_100_M"
import { B3B_PH_K_S_LF__SN_ } from "./imports/B3B_PH_K_S_LF__SN_"
import { B2B_PH_K_S_LF__SN_ } from "./imports/B2B_PH_K_S_LF__SN_"
import { BS_12_B2AA002 } from "./imports/BS_12_B2AA002"
import { LBZX84C10LT1G } from "./imports/LBZX84C10LT1G"
import { BAT54S_235 } from "./imports/BAT54S_235"
import { A_0466002_NRHF } from "./imports/A_0466002_NRHF"
import { NSMD050_24V } from "./imports/nSMD050_24V"

const N12RAW = "CAR_12V_RAW"
const N12P = "CAR_12V_PROTECTED"
const N5RAW = "BUCK_5V_RAW"
const NVBUS = "VBUS_SHARED"
const N5S = "SENSOR_5V"
const N3V3 = "V3V3_WAV"
const NSDA3 = "I2C_SDA_3V3"
const NSCL3 = "I2C_SCL_3V3"
const NSDA5 = "I2C_SDA_5V"
const NSCL5 = "I2C_SCL_5V"
const NRX = "U0RX"
const NTX = "U0TX"
const NIO16 = "GPIO16"
const GND = "GND"

const W12 = "0.8mm"
const W5 = "0.5mm"
const WSIG = "0.2mm"
const WTVS = "2mm"

const D = 46
const R = D / 2
const N = 72

export default () => {
  const circle = Array.from({ length: N }, (_, i) => {
    const a = (2 * Math.PI * i) / N
    return { x: +(R * Math.cos(a)).toFixed(3), y: +(R * Math.sin(a)).toFixed(3) }
  })

  return (
    <board width={`${D}mm`} height={`${D}mm`} layers={2} outline={circle} schAutoLayoutEnabled>
      <net name={N12RAW} isPowerNet nominalTraceWidth={W12} />
      <net name={N12P} isPowerNet nominalTraceWidth={W12} />
      <net name={N5RAW} isPowerNet nominalTraceWidth={W5} />
      <net name={NVBUS} isPowerNet nominalTraceWidth={W5} />
      <net name={N5S} isPowerNet nominalTraceWidth={W5} />
      <net name={N3V3} isPowerNet nominalTraceWidth={W5} />
      <net name={GND} isGroundNet nominalTraceWidth={W5} />
      <net name={NSDA3} nominalTraceWidth={WSIG} />
      <net name={NSCL3} nominalTraceWidth={WSIG} />
      <net name={NSDA5} nominalTraceWidth={WSIG} />
      <net name={NSCL5} nominalTraceWidth={WSIG} />

      {/* ===== A. Vehicle input / protection (left sector) ===== */}
      <B2B_PH_K_S_LF__SN_ name="J1" pcbX={-17} pcbY={11} layer="bottom" doNotPlace />
      <A_0466002_NRHF name="F1" pcbX={-14.5} pcbY={7} />
      {/* Bidirectional TVS avoids crowbarring the input during reverse-battery events. */}
      <SMBJ16CA name="D1" pcbX={-13.4} pcbY={13.7} schWidth={0.8} />
      {/* Reverse-protection P-MOS: body diode must point RAW->PROTECTED.
          D (=pin3) -> CAR_12V_RAW, S (=pin2) -> CAR_12V_PROTECTED.
          Gate pulled to GND through R1 -> Vgs = -VRAW(on); zener D3 clamps to -10 V. */}
      <DMP4065S_7 name="Q1" pcbX={-10} pcbY={9.6} schHeight={0.8} />
      <resistor name="R1" resistance="100k" footprint="0603" pcbX={-10.5} pcbY={5.5} />
      {/* D3 zener is directly across Vgs: K=source/PROTECTED, A=gate. */}
      <LBZX84C10LT1G name="D3" pcbX={-7} pcbY={6} schHeight={0.4} />

      {/* ===== B. Buck (top-left sector; keep >=8 mm from sensors/ADC) ===== */}
      <LMR36520ADDAR name="U1" pcbX={-2} pcbY={12.5} pcbRotation={270} schHeight={1} />
      <FXL0530_100_M name="L2" pcbX={6.8} pcbY={14} />
      {/* D2 = VBUS ORing: buck 5 V feeds VBUS_SHARED; USB VBUS cannot backfeed the buck */}
      <SS54 name="D2" pcbX={7} pcbY={7.5} />
      <resistor name="R2" resistance="100k" footprint="0603" pcbX={-2} pcbY={7} />
      {/* FB divider: Vout = 1.0 x (1 + 100k/24.9k) = 5.016 V (LMR36520 VREF = 1.0 V) */}
      <resistor name="R3" resistance="100k" footprint="0603" pcbX={4.7} pcbY={9.8} />
      <resistor name="R4" resistance="24.9k" footprint="0603" pcbX={1} pcbY={7.2} />
      <capacitor name="C4" capacitance="220nF" footprint="0603" maxVoltageRating="50V" supplierPartNumbers={{ jlcpcb: ["C64705"] }} pcbX={-6.9} pcbY={13.1} pcbRotation={90} />
      <capacitor name="C5" capacitance="22uF" footprint="0805" maxVoltageRating="25V" pcbX={11.8} pcbY={14} />
      <capacitor name="C21" capacitance="22uF" footprint="0805" maxVoltageRating="25V" pcbX={9.2} pcbY={16.5} />
      <capacitor name="C6" capacitance="100nF" footprint="0603" maxVoltageRating="50V" supplierPartNumbers={{ jlcpcb: ["C14663"] }} pcbX={-6.5} pcbY={8.5} pcbRotation={180} maxDecouplingTraceLength="6mm" />
      <capacitor name="C7" capacitance="100nF" footprint="0603" maxVoltageRating="25V" pcbX={12.4} pcbY={17} maxDecouplingTraceLength="12mm" />
      <capacitor name="C17" capacitance="1uF" footprint="0603" maxVoltageRating="16V" pcbX={3.6} pcbY={11.5} maxDecouplingTraceLength="15mm" />
      <capacitor name="C18" capacitance="100nF" footprint="0603" maxVoltageRating="16V" pcbX={2.8} pcbY={13.77} pcbRotation={90} schOrientation="vertical" maxDecouplingTraceLength="3mm" />
      <capacitor name="C1" capacitance="4.7uF" footprint="1206" maxVoltageRating="50V" supplierPartNumbers={{ jlcpcb: ["C29823"] }} pcbX={-3.6} pcbY={18.3} pcbRotation={90} />
      <capacitor name="C2" capacitance="4.7uF" footprint="1206" maxVoltageRating="50V" supplierPartNumbers={{ jlcpcb: ["C29823"] }} pcbX={-7.1} pcbY={16.5} pcbRotation={180} />
      {/* ===== C. VBUS path ===== */}
      {/* 500 mA hold / 1 A trip PTC provides selective sensor-branch protection. */}
      <NSMD050_24V name="F2" pcbX={6} pcbY={5} />
      <capacitor name="C8" capacitance="10uF" footprint="0603" maxVoltageRating="10V" pcbX={10} pcbY={10} />
      {/* ===== D. I2C translation (center) ===== */}
      <TCA9406DCTR name="U2" pcbX={-1.5} pcbY={-8} />
      <resistor name="R5" resistance="4.7k" footprint="0603" pcbX={-9.5} pcbY={-13.5} pcbRotation={90} />
      <resistor name="R6" resistance="4.7k" footprint="0603" pcbX={-4.4} pcbY={-13} />
      <resistor name="R7" resistance="4.7k" footprint="0603" pcbX={-1.4} pcbY={-13.8} />
      <resistor name="R8" resistance="4.7k" footprint="0603" pcbX={1.8} pcbY={-14} />
      <capacitor name="C19" capacitance="100nF" footprint="0603" maxVoltageRating="10V" supplierPartNumbers={{ jlcpcb: ["C14663"] }} pcbX={-0.35} pcbY={-12} maxDecouplingTraceLength="12mm" />
      <capacitor name="C20" capacitance="100nF" footprint="0603" maxVoltageRating="10V" supplierPartNumbers={{ jlcpcb: ["C14663"] }} pcbX={-2.65} pcbY={-3.8} pcbRotation={180} maxDecouplingTraceLength="12mm" />

      {/* ===== E. ADC + analog front end (right sector) ===== */}
      <ADS1115IDGSR name="U3" pcbX={5.1} pcbY={-10} />
      <capacitor name="C9" capacitance="100nF" footprint="0603" pcbX={7.7} pcbY={-10} pcbRotation={270} maxDecouplingTraceLength="6mm" />
      <capacitor name="C10" capacitance="1uF" footprint="0603" pcbX={9.7} pcbY={-10} pcbRotation={270} maxDecouplingTraceLength="8mm" />
      {/* BAT54S Schottky rail clamps: pin3=signal junction, pin1=GND, pin2=SENSOR_5V. */}
      <BAT54S_235 name="D4" pcbX={2} pcbY={-1.4} />
      <BAT54S_235 name="D5" pcbX={5.6} pcbY={-0.8} />
      <BAT54S_235 name="D6" pcbX={9.3} pcbY={-1.4} />
      <BAT54S_235 name="D7" pcbX={17} pcbY={-3.5} />
      {/* Per-channel RC: Jx.pin3 -> 1k -> AINx node (C11..C14 + clamp) -> U3.AINx */}
      <resistor name="R9" resistance="1k" footprint="0603" pcbX={2.2} pcbY={1.5} />
      <resistor name="R10" resistance="1k" footprint="0603" pcbX={5.6} pcbY={1.9} />
      <resistor name="R11" resistance="1k" footprint="0603" pcbX={9.3} pcbY={2.1} />
      <resistor name="R12" resistance="1k" footprint="0603" pcbX={16.8} pcbY={0.5} />
      <capacitor name="C11" capacitance="100nF" footprint="0603" pcbX={2.5} pcbY={-5.9} maxDecouplingTraceLength="25mm" />
      <capacitor name="C12" capacitance="100nF" footprint="0603" pcbX={5.6} pcbY={-5.2} maxDecouplingTraceLength="25mm" />
      <capacitor name="C13" capacitance="100nF" footprint="0603" pcbX={9.3} pcbY={-5.9} maxDecouplingTraceLength="25mm" />
      <capacitor name="C14" capacitance="100nF" footprint="0603" pcbX={16.5} pcbY={-6} maxDecouplingTraceLength="25mm" />
      {/* ===== F. Environment sensor (bottom-right, top face per decision #7) ===== */}
      <BMP280 name="U4" pcbX={15} pcbY={12.8} />
      <capacitor name="C15" capacitance="100nF" footprint="0603" pcbX={15} pcbY={15.5} pcbRotation={180} maxDecouplingTraceLength="3mm" />
      {/* ===== G. RTC (bottom-left) ===== */}
      <DS3231MZ_TRL name="U5" pcbX={-11.5} pcbY={-5} />
      <BS_12_B2AA002 name="BT1" pcbX={13.2} pcbY={-0.5} pcbRotation={90} layer="top" schWidth={0.8} doNotPlace />
      <capacitor name="C16" capacitance="100nF" footprint="0603" pcbX={-12.2} pcbY={-10.5} pcbRotation={270} maxDecouplingTraceLength="8mm" />
      {/* ===== H. Cable connectors: outward/back face, clear of the mirrored USB-C sector ===== */}
      <B3B_PH_K_S_LF__SN_ name="J2" pcbX={-17} pcbY={-9} pcbRotation={0} layer="bottom" schHeight={0.4} doNotPlace />
      <B3B_PH_K_S_LF__SN_ name="J3" pcbX={-8} pcbY={18.8} pcbRotation={0} layer="bottom" schHeight={0.4} doNotPlace />
      <B3B_PH_K_S_LF__SN_ name="J4" pcbX={5.5} pcbY={18.8} pcbRotation={0} layer="bottom" schHeight={0.4} doNotPlace />
      <B3B_PH_K_S_LF__SN_ name="J5" pcbX={18} pcbY={4} pcbRotation={180} layer="bottom" schHeight={0.4} doNotPlace />

      {/* ===== I. Waveshare mating header =====
          Mating-face transform: X_db = -X_assembly_rear.
          Assembly rear view: pin1 VBUS at +8.89 -> db x = -8.89 (pinheader pin1 is leftmost).
          SDA/SCL land on H2-6/H2-7 (GPIO17/18), i.e. db x = +3.81/+6.35. */}
      <pinheader
        name="H2"
        pinCount={8}
        pitch="2.54mm"
        gender="male"
        holeDiameter="1mm"
        platedDiameter="1.8mm"
        pcbX={0}
        pcbY={-18.68}
        layer="top"
        schWidth={0.58}
        doNotPlace
        pinLabels={{
          pin1: "VBUS",
          pin2: "GND",
          pin3: "V3V3",
          pin4: "RX",
          pin5: "TX",
          pin6: "SDA",
          pin7: "SCL",
          pin8: "IO16",
        }}
      />

      {/* Mechanical keepouts and fabrication notes. USB-C is on the mirrored -X edge. */}
      <keepout shape="rect" pcbX={-20.5} pcbY={0} width="5mm" height="10mm" layers={["top", "bottom"]} />
      <keepout shape="circle" pcbX={0} pcbY={20.5} radius="2.6mm" layers={["top", "bottom"]} />
      <keepout shape="circle" pcbX={-13.75} pcbY={-14.7} radius="2.6mm" layers={["top", "bottom"]} />
      <keepout shape="circle" pcbX={13.75} pcbY={-14.7} radius="2.6mm" layers={["top", "bottom"]} />
      <keepout shape="circle" pcbX={15} pcbY={12.8} radius="0.5mm" layer="top" excludeRefs={[".U4"]} />
      <pcbnotetext pcbX={16} pcbY={15.5} layer="top" text="BMP280: DO NOT COVER / NO COAT" fontSize="0.7mm" />
      <pcbnotetext pcbX={-18.5} pcbY={5.8} layer="top" text="USB-C KEEPOUT" fontSize="0.7mm" />

      {/* ===== mounting holes: mating spec 3x NPTH dia 2.2 ===== */}
      <hole name="MH1" diameter="2.2mm" pcbX={0} pcbY={20.5} />
      <hole name="MH2" diameter="2.2mm" pcbX={-13.75} pcbY={-14.7} />
      <hole name="MH3" diameter="2.2mm" pcbX={13.75} pcbY={-14.7} />

      {/* ===== accessible top-side probe pads ===== */}
      <testpoint name="TP_12V_RAW" pcbX={-10} pcbY={3.5} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: `net.${N12RAW}` }} />
      <testpoint name="TP_12V_PROT" pcbX={-7} pcbY={3.5} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: `net.${N12P}` }} />
      <testpoint name="TP_BUCK_5V" pcbX={-4} pcbY={5} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: `net.${N5RAW}` }} />
      <testpoint name="TP_VBUS" pcbX={-1} pcbY={5} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: `net.${NVBUS}` }} />
      <testpoint name="TP_SENSOR_5V" pcbX={9.5} pcbY={5.5} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: `net.${N5S}` }} />
      <testpoint name="TP_3V3" pcbX={11} pcbY={4.5} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: `net.${N3V3}` }} />
      <testpoint name="TP_GND" pcbX={-1} pcbY={1} layer="top" footprintVariant="pad" padDiameter="1.2mm" connections={{ pin1: `net.${GND}` }} />
      <testpoint name="TP_SDA_3V3" pcbX={-9} pcbY={-11.5} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: `net.${NSDA3}` }} />
      <testpoint name="TP_SCL_3V3" pcbX={-7} pcbY={-11.5} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: `net.${NSCL3}` }} />
      <testpoint name="TP_SDA_5V" pcbX={-5} pcbY={-11.5} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: `net.${NSDA5}` }} />
      <testpoint name="TP_SCL_5V" pcbX={-3} pcbY={-11.5} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: `net.${NSCL5}` }} />
      <testpoint name="TP_A0" pcbX={1.8} pcbY={3.3} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: ".U3 > .AIN0" }} />
      <testpoint name="TP_A1" pcbX={4.4} pcbY={3.3} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: ".U3 > .AIN1" }} />
      <testpoint name="TP_A2" pcbX={9} pcbY={4.2} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: ".U3 > .AIN2" }} />
      <testpoint name="TP_A3" pcbX={16.5} pcbY={-8} layer="top" footprintVariant="pad" padDiameter="1mm" />
      <testpoint name="TP_U0RX" pcbX={-1.27} pcbY={-15.4} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: `net.${NRX}` }} />
      <testpoint name="TP_U0TX" pcbX={1.27} pcbY={-15.4} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: `net.${NTX}` }} />
      <testpoint name="TP_GPIO16" pcbX={8.89} pcbY={-15.4} layer="top" footprintVariant="pad" padDiameter="1mm" connections={{ pin1: `net.${NIO16}` }} />

      {/* ===== netlist: A input ===== */}
      <trace name="INPUT_TO_FUSE" from=".J1 > .pin1" to=".F1 > .pin1" width={W12} />
      <trace name="INPUT_GROUND" from=".J1 > .pin2" to={`net.${GND}`} width={W12} />
      <trace name="FUSED_12V" from=".F1 > .pin2" to={`net.${N12RAW}`} width={W12} />
      <trace name="TVS_INPUT" from=".D1 > .pin1" to={`net.${N12RAW}`} width={WTVS} />
      <trace name="TVS_GROUND" from=".D1 > .pin2" to={`net.${GND}`} width={WTVS} />
      <trace name="PMOS_DRAIN" from=".Q1 > .D" to={`net.${N12RAW}`} width={W12} />
      <trace name="PMOS_SOURCE" from=".Q1 > .S" to={`net.${N12P}`} width={W12} />
      <trace name="PMOS_GATE_PULL" from=".Q1 > .G" to=".R1 > .pin1" width={WSIG} />
      <trace name="PMOS_GATE_GROUND" from=".R1 > .pin2" to={`net.${GND}`} width={WSIG} />
      <trace name="PMOS_VGS_CLAMP_K" from=".D3 > .K" to={`net.${N12P}`} width={WSIG} />
      <trace name="PMOS_VGS_CLAMP_A" from=".D3 > .A" to=".Q1 > .G" width={WSIG} />
      <trace name="VIN_BULK_1" from=".C1 > .pin1" to={`net.${N12P}`} width={W12} maxLength="10mm" />
      <trace name="VIN_BULK_1_GND" from=".C1 > .pin2" to={`net.${GND}`} width={W12} />
      <trace name="VIN_BULK_2" from=".C2 > .pin1" to={`net.${N12P}`} width={W12} maxLength="9mm" />
      <trace name="VIN_BULK_2_GND" from=".C2 > .pin2" to={`net.${GND}`} width={W12} />
      {/* ===== netlist: B buck ===== */}
      <trace name="BUCK_VIN" from=".U1 > .VIN" to=".C4 > .pin1" width={W12} maxLength="6mm" pcbPath={[".C4 > .pin1"]} />
      <trace name="BUCK_PGND" from=".U1 > .PGND" to={`net.${GND}`} width={W12} />
      <trace name="BUCK_ENABLE" from=".U1 > .EN" to={`net.${N12P}`} width={WSIG} />
      <trace name="BUCK_POWER_GOOD" from=".U1 > .PG" to=".R2 > .pin1" width={WSIG} />
      <trace name="BUCK_POWER_GOOD_PULLUP" from=".R2 > .pin2" to={`net.${N5RAW}`} width={WSIG} />
      <trace name="BUCK_VCC_CAP" from=".U1 > .VCC" to=".C17 > .pin1" width={W5} maxLength="4mm" />
      <trace name="BUCK_VCC_CAP_GND" from=".C17 > .pin2" to={`net.${GND}`} width={W5} />
      <trace name="BUCK_EXPOSED_PAD" from=".U1 > .EP" to={`net.${GND}`} width={W12} />
      <trace name="BUCK_SWITCH_NODE" from=".U1 > .SW" to=".L2 > .pin1" width={W12} maxLength="7mm" />
      <trace name="BUCK_OUTPUT" from=".L2 > .pin2" to={`net.${N5RAW}`} width={W12} />
      {/* BOOT cap only: synchronous converter, boot switch is internal */}
      <trace name="BUCK_BOOT" from=".U1 > .BOOT" to=".C18 > .pin1" width={W5} maxLength="3mm" />
      <trace name="BUCK_BOOT_SW" from=".C18 > .pin2" to=".U1 > .SW" width={W5} maxLength="3mm" />
      <trace name="BUCK_FB_SENSE" from=".U1 > .FB" to=".R3 > .pin1" width={WSIG} maxLength="4mm" pcbStraightLine />
      <trace name="BUCK_FB_HIGH" from=".R3 > .pin2" to=".C5 > .pin1" width={WSIG} maxLength="10mm" pcbPath={[".C5 > .pin1"]} />
      <trace name="BUCK_FB_LOW" from=".R3 > .pin1" to=".R4 > .pin1" width={WSIG} />
      <trace name="BUCK_FB_GROUND" from=".R4 > .pin2" to=".U1 > .EP" width={WSIG} />
      <trace name="BUCK_HF_INPUT" from=".C4 > .pin1" to={`net.${N12P}`} width={W12} maxLength="3mm" />
      <trace
        name="BUCK_HF_INPUT_GND"
        from=".C4 > .pin2"
        to=".U1 > .PGND"
        width={W12}
        maxLength="3mm"
        pcbPath={[".U1 > .PGND"]}
      />
      <trace name="BUCK_OUTPUT_CAP" from=".C5 > .pin1" to=".L2 > .pin2" width={W12} maxLength="7mm" pcbPath={[".L2 > .pin2"]} />
      <trace name="BUCK_OUTPUT_CAP_GND" from=".C5 > .pin2" to={`net.${GND}`} width={W12} />
      <trace name="BUCK_OUTPUT_CAP_2" from=".C21 > .pin1" to=".L2 > .pin2" width={W12} maxLength="7mm" pcbPath={[".L2 > .pin2"]} />
      <trace name="BUCK_OUTPUT_CAP_2_GND" from=".C21 > .pin2" to={`net.${GND}`} width={W12} />
      <trace name="BUCK_INPUT_BYPASS" from=".C6 > .pin1" to={`net.${N12P}`} width={W5} maxLength="4mm" />
      <trace name="BUCK_INPUT_BYPASS_GND" from=".C6 > .pin2" to={`net.${GND}`} width={W5} />
      <trace name="BUCK_OUTPUT_BYPASS" from=".C7 > .pin1" to=".C5 > .pin1" width={W5} maxLength="8mm" pcbPath={[".C5 > .pin1"]} />
      <trace name="BUCK_OUTPUT_BYPASS_GND" from=".C7 > .pin2" to={`net.${GND}`} width={W5} />

      {/* ===== netlist: C VBUS ORing / sensor rail ===== */}
      <trace name="BUCK_ORING_ANODE" from=".D2 > .pos" to={`net.${N5RAW}`} width={W5} />
      <trace name="BUCK_ORING_CATHODE" from=".D2 > .neg" to={`net.${NVBUS}`} width={W5} />
      <trace name="SENSOR_FUSE_INPUT" from=".F2 > .pin1" to={`net.${NVBUS}`} width={W5} />
      <trace name="SENSOR_FUSE_OUTPUT" from=".F2 > .pin2" to={`net.${N5S}`} width={W5} pcbRouteHints={[{ x: 8.5, y: 5 }, { x: 9.5, y: 5.5 }]} />
      <trace name="SENSOR_RAIL_CAP" from=".C8 > .pin1" to={`net.${N5S}`} width={W5} />
      <trace name="SENSOR_RAIL_CAP_GND" from=".C8 > .pin2" to={`net.${GND}`} width={W5} />

      {/* ===== netlist: I mating header ===== */}
      <trace name="H2_VBUS" from=".H2 > .pin1" to={`net.${NVBUS}`} width={W5} />
      <trace name="H2_GROUND" from=".H2 > .pin2" to={`net.${GND}`} width={W5} />
      <trace name="H2_3V3" from=".H2 > .pin3" to={`net.${N3V3}`} width={W5} />
      <trace name="H2_RX" from=".H2 > .pin4" to={`net.${NRX}`} width={WSIG} />
      <trace name="H2_TX" from=".H2 > .pin5" to={`net.${NTX}`} width={WSIG} />
      <trace name="H2_SDA" from=".H2 > .pin6" to={`net.${NSDA3}`} width={WSIG} />
      <trace name="H2_SCL" from=".H2 > .pin7" to={`net.${NSCL3}`} width={WSIG} />
      <trace name="H2_GPIO16" from=".H2 > .pin8" to={`net.${NIO16}`} width={WSIG} />

      {/* ===== netlist: D I2C translation ===== */}
      <trace name="LEVEL_VCCA" from=".U2 > .VCCA" to={`net.${N3V3}`} width={W5} />
      <trace name="LEVEL_VCCB" from=".U2 > .VCCB" to={`net.${N5S}`} width={W5} />
      <trace name="LEVEL_GROUND" from=".U2 > .GND" to={`net.${GND}`} width={W5} />
      <trace name="LEVEL_ENABLE" from=".U2 > .OE" to={`net.${N3V3}`} width={WSIG} />
      <trace name="LEVEL_SDA_A" from=".U2 > .SDA_A" to={`net.${NSDA3}`} width={WSIG} />
      <trace name="LEVEL_SCL_A" from=".U2 > .SCL_A" to={`net.${NSCL3}`} width={WSIG} />
      <trace name="LEVEL_SDA_B" from=".U2 > .SDA_B" to={`net.${NSDA5}`} width={WSIG} />
      <trace name="LEVEL_SCL_B" from=".U2 > .SCL_B" to={`net.${NSCL5}`} width={WSIG} />
      <trace name="SDA_3V3_PULLUP" from=".R5 > .pin1" to={`net.${NSDA3}`} width={WSIG} />
      <trace name="SDA_3V3_PULLUP_RAIL" from=".R5 > .pin2" to={`net.${N3V3}`} width={WSIG} />
      <trace name="SCL_3V3_PULLUP" from=".R6 > .pin1" to={`net.${NSCL3}`} width={WSIG} />
      <trace name="SCL_3V3_PULLUP_RAIL" from=".R6 > .pin2" to={`net.${N3V3}`} width={WSIG} />
      <trace name="SDA_5V_PULLUP" from=".R7 > .pin1" to={`net.${NSDA5}`} width={WSIG} />
      <trace name="SDA_5V_PULLUP_RAIL" from=".R7 > .pin2" to={`net.${N5S}`} width={WSIG} />
      <trace name="SCL_5V_PULLUP" from=".R8 > .pin1" to={`net.${NSCL5}`} width={WSIG} />
      <trace name="SCL_5V_PULLUP_RAIL" from=".R8 > .pin2" to={`net.${N5S}`} width={WSIG} />
      <trace name="LEVEL_VCCA_BYPASS" from=".C19 > .pin1" to=".U2 > .VCCA" width={W5} maxLength="4mm" pcbPath={[".U2 > .VCCA"]} />
      <trace name="LEVEL_VCCA_BYPASS_GND" from=".C19 > .pin2" to={`net.${GND}`} width={W5} />
      <trace name="LEVEL_VCCB_BYPASS" from=".C20 > .pin1" to=".U2 > .VCCB" width={W5} maxLength="4mm" />
      <trace name="LEVEL_VCCB_BYPASS_GND" from=".C20 > .pin2" to={`net.${GND}`} width={W5} />

      {/* ===== netlist: E ADC + per-channel RC filters ===== */}
      <trace from=".U3 > .VDD" to={`net.${N5S}`} />
      <trace from=".U3 > .GND" to={`net.${GND}`} />
      <trace from=".U3 > .ADDR" to={`net.${GND}`} />
      <trace name="ADC_SDA" from=".U3 > .SDA" to={`net.${NSDA5}`} width={WSIG} pcbRouteHints={[{ x: 4.6, y: -6.8 }, { x: 3, y: -6.8 }]} />
      <trace name="ADC_SCL" from=".U3 > .SCL" to={`net.${NSCL5}`} width={WSIG} pcbRouteHints={[{ x: 4.1, y: -7.3 }, { x: 3, y: -7.3 }]} />
      <trace name="ADC_A0" from=".U3 > .AIN0" to=".R9 > .pin2" width={WSIG} />
      <trace name="SENSOR_A0" from=".R9 > .pin1" to=".J2 > .pin3" width={WSIG} />
      <trace name="ADC_A0_FILTER" from=".R9 > .pin2" to=".C11 > .pin1" width={WSIG} />
      <trace name="ADC_A0_FILTER_GND" from=".C11 > .pin2" to={`net.${GND}`} width={WSIG} />
      <trace name="ADC_A0_CLAMP_NODE" from=".R9 > .pin2" to=".D4 > .pin3" width={WSIG} />
      <trace name="ADC_A0_CLAMP_LOW" from=".D4 > .pin1" to={`net.${GND}`} width={WSIG} />
      <trace name="ADC_A0_CLAMP_HIGH" from=".D4 > .pin2" to={`net.${N5S}`} width={WSIG} />
      <trace name="ADC_A1" from=".U3 > .AIN1" to=".R10 > .pin2" width={WSIG} />
      <trace name="SENSOR_A1" from=".R10 > .pin1" to=".J3 > .pin3" width={WSIG} pcbRouteHints={[{ x: -8, y: 17 }, { x: -6, y: 12 }]} />
      <trace name="ADC_A1_FILTER" from=".R10 > .pin2" to=".C12 > .pin1" width={WSIG} />
      <trace name="ADC_A1_FILTER_GND" from=".C12 > .pin2" to={`net.${GND}`} width={WSIG} />
      <trace name="ADC_A1_CLAMP_NODE" from=".R10 > .pin2" to=".D5 > .pin3" width={WSIG} />
      <trace name="ADC_A1_CLAMP_LOW" from=".D5 > .pin1" to={`net.${GND}`} width={WSIG} />
      <trace name="ADC_A1_CLAMP_HIGH" from=".D5 > .pin2" to={`net.${N5S}`} width={WSIG} />
      <trace name="ADC_A2" from=".U3 > .AIN2" to=".R11 > .pin2" width={WSIG} />
      <trace name="SENSOR_A2" from=".R11 > .pin1" to=".J4 > .pin3" width={WSIG} pcbRouteHints={[{ x: 7.5, y: 4 }, { x: 7.5, y: 12 }, { x: 7.5, y: 17 }]} />
      <trace name="ADC_A2_FILTER" from=".R11 > .pin2" to=".C13 > .pin1" width={WSIG} />
      <trace name="ADC_A2_FILTER_GND" from=".C13 > .pin2" to={`net.${GND}`} width={WSIG} />
      <trace name="ADC_A2_CLAMP_NODE" from=".R11 > .pin2" to=".D6 > .pin3" width={WSIG} />
      <trace name="ADC_A2_CLAMP_LOW" from=".D6 > .pin1" to={`net.${GND}`} width={WSIG} />
      <trace name="ADC_A2_CLAMP_HIGH" from=".D6 > .pin2" to={`net.${N5S}`} width={WSIG} />
      <trace
        name="ADC_A3"
        from=".U3 > .AIN3"
        to=".R12 > .pin2"
        width={WSIG}
        pcbPath={[
          { x: 0.5, y: 2.9 },
          { x: 0.5, y: 2.9, via: true, fromLayer: "top", toLayer: "bottom" },
          { x: 0.5, y: 2.9 },
          { x: 2.9, y: 3.7 },
          { x: 6.9, y: 5 },
          { x: 12.9, y: 9 },
          { x: 12.9, y: 9, via: true, fromLayer: "bottom", toLayer: "top" },
          { x: 12.9, y: 9 },
        ]}
      />
      <trace name="TP_ADC_A3" from=".TP_A3 > .pin1" to=".R12 > .pin2" width={WSIG} />
      <trace name="SENSOR_A3" from=".R12 > .pin1" to=".J5 > .pin3" width={WSIG} />
      <trace name="ADC_A3_FILTER" from=".R12 > .pin2" to=".C14 > .pin1" width={WSIG} />
      <trace name="ADC_A3_FILTER_GND" from=".C14 > .pin2" to={`net.${GND}`} width={WSIG} />
      <trace name="ADC_A3_CLAMP_NODE" from=".R12 > .pin2" to=".D7 > .pin3" width={WSIG} />
      <trace name="ADC_A3_CLAMP_LOW" from=".D7 > .pin1" to={`net.${GND}`} width={WSIG} />
      <trace name="ADC_A3_CLAMP_HIGH" from=".D7 > .pin2" to={`net.${N5S}`} width={WSIG} />
      <trace
        name="ADC_BYPASS_100N"
        from=".C9 > .pin1"
        to=".U3 > .VDD"
        width={WSIG}
        maxLength="6mm"
        pcbPathRelativeTo=".U3 > .VDD"
        pcbPath={[
          { x: 0, y: 3.35 },
          { x: 1.45, y: 3.35 },
        ]}
      />
      <trace
        name="ADC_BYPASS_100N_GND"
        from=".C9 > .pin2"
        to=".U3 > .GND"
        width={WSIG}
        maxLength="6mm"
        pcbPathRelativeTo=".U3 > .GND"
        pcbPath={[
          { x: 0, y: -3.35 },
          { x: 1.45, y: -3.35 },
        ]}
      />
      <trace name="ADC_BYPASS_1U" from=".C10 > .pin1" to=".U3 > .VDD" width={WSIG} />
      <trace name="ADC_BYPASS_1U_GND" from=".C10 > .pin2" to=".U3 > .GND" width={WSIG} />

      {/* ===== netlist: F BMP280 ===== */}
      <trace name="ENV_VDD" from=".U4 > .VDD" to={`net.${N3V3}`} width={WSIG} pcbRouteHints={[{ x: 16.6, y: 15.975 }, { x: 16.9, y: 15.2 }]} />
      <trace name="ENV_VDDIO" from=".U4 > .VDDIO" to={`net.${N3V3}`} width={WSIG} pcbRouteHints={[{ x: 16.6, y: 14.675 }, { x: 16.9, y: 13.8 }]} />
      <trace name="ENV_CSB" from=".U4 > .CSB" to={`net.${N3V3}`} width={WSIG} pcbRouteHints={[{ x: 13.4, y: 15.325 }, { x: 13.1, y: 16.1 }]} />
      <trace name="ENV_GND1" from=".U4 > .GND1" to={`net.${GND}`} width={WSIG} pcbRouteHints={[{ x: 16.6, y: 15.325 }, { x: 17, y: 16.1 }]} />
      <trace name="ENV_GND2" from=".U4 > .GND2" to={`net.${GND}`} width={WSIG} pcbRouteHints={[{ x: 13.4, y: 15.975 }, { x: 13.1, y: 16.5 }]} />
      <trace name="ENV_SDO_GROUND" from=".U4 > .SDO" to={`net.${GND}`} width={WSIG} pcbRouteHints={[{ x: 16.6, y: 14.025 }, { x: 17, y: 13.1 }]} />
      <trace name="ENV_SDA" from=".U4 > .SDI" to={`net.${NSDA3}`} width={WSIG} pcbRouteHints={[{ x: 13.4, y: 14.675 }, { x: 13.4, y: 11 }]} />
      <trace name="ENV_SCL" from=".U4 > .SCK" to={`net.${NSCL3}`} width={WSIG} pcbRouteHints={[{ x: 13.4, y: 14.025 }, { x: 11.6, y: 14.025 }, { x: 11.6, y: 10.5 }]} />
      <trace name="ENV_VDD_DECOUPLE" from=".C15 > .pin1" to=".U4 > .VDD" width={WSIG} maxLength="3mm" pcbStraightLine />
      <trace name="ENV_GND_DECOUPLE" from=".C15 > .pin2" to=".U4 > .GND2" width={WSIG} maxLength="3mm" pcbStraightLine />

      {/* ===== netlist: G RTC ===== */}
      <trace from=".U5 > .VCC" to={`net.${N3V3}`} />
      <trace from=".U5 > .GND" to={`net.${GND}`} />
      <trace from=".U5 > .SDA" to={`net.${NSDA3}`} />
      <trace from=".U5 > .SCL" to={`net.${NSCL3}`} />
      <trace
        name="RTC_BACKUP_BATTERY"
        from=".U5 > .VBAT"
        to=".BT1 > ._POS"
        pcbRouteHints={[
          { x: -8, y: -0.5 },
          { x: 0, y: 0 },
          { x: 9, y: 0 },
          { x: 11.5, y: 5 },
        ]}
      />
      <trace name="RTC_BACKUP_GROUND" from=".BT1 > ._NEG" to={`net.${GND}`} />
      <trace name="RTC_VCC_DECOUPLE" from=".C16 > .pin1" to=".U5 > .VCC" width={WSIG} maxLength="3mm" pcbStraightLine />
      <trace name="RTC_GND_DECOUPLE" from=".C16 > .pin2" to={`net.${GND}`} width={WSIG} />
      {/* 32kHz / N_RST deliberately left open on the MZ */}

      {/* ===== netlist: H sensor connectors (pin1=+5V, pin2=GND, pin3=signal) ===== */}
      <trace from=".J2 > .pin1" to={`net.${N5S}`} />
      <trace from=".J2 > .pin2" to={`net.${GND}`} />
      <trace name="J3_SENSOR_5V" from=".J3 > .pin1" to={`net.${N5S}`} width={W5} pcbRouteHints={[{ x: -8, y: 16.5 }, { x: -8, y: 12 }]} />
      <trace name="J3_GROUND" from=".J3 > .pin2" to={`net.${GND}`} width={W5} pcbRouteHints={[{ x: -7, y: 16.5 }, { x: -7, y: 12 }]} />
      <trace name="J4_SENSOR_5V" from=".J4 > .pin1" to={`net.${N5S}`} width={W5} pcbRouteHints={[{ x: 3.5, y: 16.5 }, { x: 4, y: 12 }]} />
      <trace name="J4_GROUND" from=".J4 > .pin2" to={`net.${GND}`} width={W5} pcbRouteHints={[{ x: 5.5, y: 16.5 }, { x: 6, y: 12 }]} />
      <trace from=".J5 > .pin1" to={`net.${N5S}`} />
      <trace from=".J5 > .pin2" to={`net.${GND}`} />

      {/* Ground planes and thermal/stitching vias. The four central vias sit in U1's exposed-pad area. */}
      <via name="U1_TH1" pcbX={-2.6} pcbY={11.9} fromLayer="top" toLayer="bottom" outerDiameter="0.65mm" holeDiameter="0.3mm" connectsTo={`net.${GND}`} />
      <via name="U1_TH2" pcbX={-1.4} pcbY={11.9} fromLayer="top" toLayer="bottom" outerDiameter="0.65mm" holeDiameter="0.3mm" connectsTo={`net.${GND}`} />
      <via name="U1_TH3" pcbX={-2.6} pcbY={13.1} fromLayer="top" toLayer="bottom" outerDiameter="0.65mm" holeDiameter="0.3mm" connectsTo={`net.${GND}`} />
      <via name="U1_TH4" pcbX={-1.4} pcbY={13.1} fromLayer="top" toLayer="bottom" outerDiameter="0.65mm" holeDiameter="0.3mm" connectsTo={`net.${GND}`} />
      <via name="GND_STITCH_1" pcbX={-12} pcbY={2} fromLayer="top" toLayer="bottom" outerDiameter="0.8mm" holeDiameter="0.4mm" connectsTo={`net.${GND}`} />
      <via name="GND_STITCH_2" pcbX={-9} pcbY={-1} fromLayer="top" toLayer="bottom" outerDiameter="0.8mm" holeDiameter="0.4mm" connectsTo={`net.${GND}`} />
      <via name="GND_STITCH_4" pcbX={12} pcbY={2.5} fromLayer="top" toLayer="bottom" outerDiameter="0.8mm" holeDiameter="0.4mm" connectsTo={`net.${GND}`} />
      <via name="GND_STITCH_5" pcbX={15} pcbY={-8} fromLayer="top" toLayer="bottom" outerDiameter="0.8mm" holeDiameter="0.4mm" connectsTo={`net.${GND}`} />
      <via name="GND_C19" pcbX={1.5} pcbY={-12} fromLayer="top" toLayer="bottom" outerDiameter="0.8mm" holeDiameter="0.4mm" connectsTo={`net.${GND}`} />
      <via name="GND_C21" pcbX={9.3} pcbY={17.6} fromLayer="top" toLayer="bottom" outerDiameter="0.8mm" holeDiameter="0.4mm" connectsTo={`net.${GND}`} />
      <copperpour name="GND_TOP" layer="top" connectsTo={`net.${GND}`} clearance="0.2mm" boardEdgeMargin="0.3mm" useThermalReliefs />
      <copperpour name="GND_BOTTOM" layer="bottom" connectsTo={`net.${GND}`} clearance="0.2mm" boardEdgeMargin="0.3mm" useThermalReliefs />
    </board>
  )
}
