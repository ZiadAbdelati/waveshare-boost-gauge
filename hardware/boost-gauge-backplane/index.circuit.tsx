import { LMR36520ADDAR } from "./imports/LMR36520ADDAR"
import { TCA9406DCTR } from "./imports/TCA9406DCTR"
import { ADS1115IDGSR } from "./imports/ADS1115IDGSR"
import { BMP280 } from "./imports/BMP280"
import { DS3231MZ_TRL } from "./imports/DS3231MZ_TRL"
import { AO3401A } from "./imports/AO3401A"
import { SMBJ16A } from "./imports/SMBJ16A"
import { SS54 } from "./imports/SS54"
import { FXL0530_4R7_M } from "./imports/FXL0530_4R7_M"
import { B3B_PH_K_S_LF__SN_ } from "./imports/B3B_PH_K_S_LF__SN_"
import { B2B_PH_K_S_LF__SN_ } from "./imports/B2B_PH_K_S_LF__SN_"
import { BS_12_B2AA002 } from "./imports/BS_12_B2AA002"
import { LBZX84C10LT1G } from "./imports/LBZX84C10LT1G"
import { BAV99 } from "./imports/BAV99"
import { A_0466002_NRHF } from "./imports/A_0466002_NRHF"
import { BSMD1206_200_12V } from "./imports/BSMD1206_200_12V"

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
const GND = "GND"

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
      {/* ===== A. Vehicle input / protection (left sector) ===== */}
      <B2B_PH_K_S_LF__SN_ name="J1" pcbX={-19} pcbY={5.5} doNotPlace />
      <A_0466002_NRHF name="F1" pcbX={-18.2} pcbY={9.5} />
      <SMBJ16A name="D1" pcbX={-12} pcbY={9.5} schWidth={0.6} />
      {/* Reverse-protection P-MOS: body diode must point RAW->PROTECTED.
          D (=pin3) -> CAR_12V_RAW, S (=pin2) -> CAR_12V_PROTECTED.
          Gate pulled to GND through R1 -> Vgs = -VRAW(on); zener D3 clamps to -10 V (AO3401A abs max +/-12 V). */}
      <AO3401A name="Q1" pcbX={-8.5} pcbY={12} schHeight={0.8} />
      <resistor name="R1" resistance="100k" footprint="0603" pcbX={-11} pcbY={5.5} />
      {/* D3 zener: reverse across Vgs (K=source=RAW, A=gate) */}
      <LBZX84C10LT1G name="D3" pcbX={-6} pcbY={6.5} />

      {/* ===== B. Buck (top-left sector; keep >=8 mm from sensors/ADC) ===== */}
      <LMR36520ADDAR name="U1" pcbX={-3} pcbY={12.5} />
      <FXL0530_4R7_M name="L2" pcbX={4} pcbY={12.5} />
      {/* D2 = VBUS ORing: buck 5 V feeds VBUS_SHARED; USB VBUS cannot backfeed the buck */}
      <SS54 name="D2" pcbX={4} pcbY={9} />
      <resistor name="R2" resistance="100k" footprint="0603" pcbX={-11.8} pcbY={17.2} />
      {/* FB divider: Vout = 1.0 x (1 + 100k/24.9k) = 5.016 V (LMR36520 VREF = 1.0 V) */}
      <resistor name="R3" resistance="100k" footprint="0603" pcbX={-8.2} pcbY={18.6} />
      <resistor name="R4" resistance="24.9k" footprint="0603" pcbX={-5} pcbY={19.2} />
      <capacitor name="C4" capacitance="22uF" footprint="0805" voltage="25V" pcbX={2.2} pcbY={17.6} maxDecouplingTraceLength="25mm" />
      <capacitor name="C5" capacitance="22uF" footprint="0805" voltage="25V" pcbX={5.6} pcbY={18} maxDecouplingTraceLength="25mm" />
      <capacitor name="C6" capacitance="100nF" footprint="0603" pcbX={9} pcbY={16.5} maxDecouplingTraceLength="25mm" />
      <capacitor name="C7" capacitance="100nF" footprint="0603" pcbX={12.5} pcbY={16} maxDecouplingTraceLength="25mm" />
      <capacitor name="C17" capacitance="1uF" footprint="0603" pcbX={-1.5} pcbY={18.5} maxDecouplingTraceLength="5mm" />
      <capacitor name="C18" capacitance="100nF" footprint="0603" pcbX={-2} pcbY={6.5} maxDecouplingTraceLength="25mm" />
      <capacitor name="C1" capacitance="22uF" footprint="1210" voltage="50V" pcbX={-13.8} pcbY={13.8} maxDecouplingTraceLength="25mm" />
      <capacitor name="C2" capacitance="22uF" footprint="1210" voltage="50V" pcbX={2} pcbY={5.5} maxDecouplingTraceLength="25mm" />
      <capacitor name="C3" capacitance="100nF" footprint="0603" pcbX={-7.5} pcbY={3.5} maxDecouplingTraceLength="25mm" />
      {/* ===== C. VBUS path ===== */}
      <BSMD1206_200_12V name="F2" pcbX={11} pcbY={12} />
      <capacitor name="C8" capacitance="10uF" footprint="0603" pcbX={14.5} pcbY={8.66} maxDecouplingTraceLength="25mm" />
      {/* ===== D. I2C translation (center) ===== */}
      <TCA9406DCTR name="U2" pcbX={-1.5} pcbY={-8} />
      <resistor name="R5" resistance="4.7k" footprint="0603" pcbX={-7.6} pcbY={-13} />
      <resistor name="R6" resistance="4.7k" footprint="0603" pcbX={-4.4} pcbY={-13} />
      <resistor name="R7" resistance="4.7k" footprint="0603" pcbX={-1.2} pcbY={-13} />
      <resistor name="R8" resistance="4.7k" footprint="0603" pcbX={1.8} pcbY={-14} />

      {/* ===== E. ADC + analog front end (right sector) ===== */}
      <ADS1115IDGSR name="U3" pcbX={5.1} pcbY={-10} />
      <capacitor name="C9" capacitance="100nF" footprint="0603" pcbX={4.8} pcbY={-14.5} maxDecouplingTraceLength="25mm" />
      <capacitor name="C10" capacitance="1uF" footprint="0603" pcbX={8.2} pcbY={-14.5} maxDecouplingTraceLength="25mm" />
      {/* BAV99 series-pair clamps: pin2 = signal junction, pin1 (D1 A) -> GND low clamp, pin3 (D2 K) -> SENSOR_5V high clamp */}
      <BAV99 name="D4" pcbX={2} pcbY={-1.4} />
      <BAV99 name="D5" pcbX={5.6} pcbY={-1.4} />
      <BAV99 name="D6" pcbX={9.3} pcbY={-1.4} />
      <BAV99 name="D7" pcbX={16.8} pcbY={-3} />
      {/* Per-channel RC: Jx.pin3 -> 1k -> AINx node (C11..C14 + clamp) -> U3.AINx */}
      <resistor name="R9" resistance="1k" footprint="0603" pcbX={2.2} pcbY={1.5} />
      <resistor name="R10" resistance="1k" footprint="0603" pcbX={5.6} pcbY={1.5} />
      <resistor name="R11" resistance="1k" footprint="0603" pcbX={9.3} pcbY={1.5} />
      <resistor name="R12" resistance="1k" footprint="0603" pcbX={16.8} pcbY={0.5} />
      <capacitor name="C11" capacitance="100nF" footprint="0603" pcbX={1.9} pcbY={-5.9} maxDecouplingTraceLength="25mm" />
      <capacitor name="C12" capacitance="100nF" footprint="0603" pcbX={5.6} pcbY={-5.2} maxDecouplingTraceLength="25mm" />
      <capacitor name="C13" capacitance="100nF" footprint="0603" pcbX={9.3} pcbY={-5.9} maxDecouplingTraceLength="25mm" />
      <capacitor name="C14" capacitance="100nF" footprint="0603" pcbX={16.5} pcbY={-6} maxDecouplingTraceLength="25mm" />
      {/* ===== F. Environment sensor (bottom-right, top face per decision #7) ===== */}
      <BMP280 name="U4" pcbX={15.5} pcbY={12.5} />
      <capacitor name="C15" capacitance="100nF" footprint="0603" pcbX={11.5} pcbY={14} maxDecouplingTraceLength="25mm" />
      {/* ===== G. RTC (bottom-left) ===== */}
      <DS3231MZ_TRL name="U5" pcbX={-11.5} pcbY={-5} />
      <BS_12_B2AA002 name="BT1" pcbX={13.2} pcbY={-3} pcbRotation={90} schWidth={0.8} doNotPlace />
      <capacitor name="C16" capacitance="100nF" footprint="0603" pcbX={-7} pcbY={-5} maxDecouplingTraceLength="25mm" />
      {/* ===== H. Sensor connectors (bottom edge, outward face) ===== */}
      <B3B_PH_K_S_LF__SN_ name="J2" pcbX={-18.6} pcbY={1} doNotPlace />
      <B3B_PH_K_S_LF__SN_ name="J3" pcbX={-19} pcbY={-2.5} doNotPlace />
      <B3B_PH_K_S_LF__SN_ name="J4" pcbX={-18.6} pcbY={-9} pcbRotation={90} doNotPlace />
      <B3B_PH_K_S_LF__SN_ name="J5" pcbX={19} pcbY={3} doNotPlace />

      {/* ===== I. Waveshare mating header =====
          Mating-face transform: X_db = -X_assembly_rear.
          Assembly rear view: pin1 VBUS at +8.89 -> db x = -8.89 (pinheader pin1 is leftmost).
          SDA/SCL land on H2-6/H2-7 (GPIO17/18), i.e. db x = +3.81/+6.35. */}
      <chip
        name="H2"
        pinCount={8}
        footprint="pinheader8"
        pcbX={0}
        pcbY={-18.68}
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
        pinAttributes={{
          pin4: { doNotConnect: true },
          pin5: { doNotConnect: true },
          pin8: { doNotConnect: true },
        }}
      />

      {/* ===== mounting holes: mating spec 3x NPTH dia 2.2 ===== */}
      <hole name="MH1" diameter="2.2mm" pcbX={0} pcbY={20.5} />
      <hole name="MH2" diameter="2.2mm" pcbX={-13.75} pcbY={-14.7} />
      <hole name="MH3" diameter="2.2mm" pcbX={13.75} pcbY={-14.7} />

      {/* ===== netlist: A input ===== */}
      <trace from=".J1 > .pin1" to={`net.${N12RAW}`} />
      <trace from=".J1 > .pin2" to={`net.${GND}`} />
      <trace from=".F1 > .pin1" to={`net.${N12RAW}`} />
      <trace from=".F1 > .pin2" to={`net.${N12P}`} />
      <trace from=".D1 > .C" to={`net.${N12P}`} />
      <trace from=".D1 > .A" to={`net.${GND}`} />
      <trace from=".Q1 > .D" to={`net.${N12RAW}`} />
      <trace from=".Q1 > .S" to={`net.${N12P}`} />
      <trace from=".Q1 > .G" to=".R1 > .pin1" />
      <trace from=".R1 > .pin2" to={`net.${GND}`} />
      <trace from=".D3 > .K" to={`net.${N12RAW}`} />
      <trace from=".D3 > .A" to=".Q1 > .G" />
      <trace from=".C1 > .pin1" to={`net.${N12RAW}`} />
      <trace from=".C1 > .pin2" to={`net.${GND}`} />
      <trace from=".C2 > .pin1" to={`net.${N12P}`} />
      <trace from=".C2 > .pin2" to={`net.${GND}`} />
      <trace from=".C3 > .pin1" to={`net.${N12P}`} />
      <trace from=".C3 > .pin2" to={`net.${GND}`} />

      {/* ===== netlist: B buck ===== */}
      <trace from=".U1 > .VIN" to={`net.${N12P}`} />
      <trace from=".U1 > .PGND" to={`net.${GND}`} />
      <trace from=".U1 > .EN" to={`net.${N12P}`} />
      <trace from=".U1 > .PG" to=".R2 > .pin1" />
      <trace from=".R2 > .pin2" to={`net.${N5RAW}`} />
      <trace from=".U1 > .VCC" to=".C17 > .pin1" />
      <trace from=".C17 > .pin2" to={`net.${GND}`} />
      <trace from=".U1 > .EP" to={`net.${GND}`} />
      <trace from=".U1 > .SW" to=".L2 > .pin1" />
      <trace from=".L2 > .pin2" to={`net.${N5RAW}`} />
      {/* BOOT cap only: synchronous converter, boot switch is internal */}
      <trace from=".U1 > .BOOT" to=".C18 > .pin1" />
      <trace from=".C18 > .pin2" to=".U1 > .SW" />
      <trace from=".U1 > .FB" to=".R3 > .pin1" />
      <trace from=".R3 > .pin2" to={`net.${N5RAW}`} />
      <trace from=".U1 > .FB" to=".R4 > .pin1" />
      <trace from=".R4 > .pin2" to={`net.${GND}`} />
      <trace from=".C4 > .pin1" to={`net.${N12P}`} />
      <trace from=".C4 > .pin2" to={`net.${GND}`} />
      <trace from=".C5 > .pin1" to={`net.${N5RAW}`} />
      <trace from=".C5 > .pin2" to={`net.${GND}`} />
      <trace from=".C6 > .pin1" to={`net.${N12P}`} />
      <trace from=".C6 > .pin2" to={`net.${GND}`} />
      <trace from=".C7 > .pin1" to={`net.${N5RAW}`} />
      <trace from=".C7 > .pin2" to={`net.${GND}`} />

      {/* ===== netlist: C VBUS ORing / sensor rail ===== */}
      <trace from=".D2 > .pos" to={`net.${N5RAW}`} />
      <trace from=".D2 > .neg" to={`net.${NVBUS}`} />
      <trace from=".F2 > .pin1" to={`net.${NVBUS}`} />
      <trace from=".F2 > .pin2" to={`net.${N5S}`} />
      <trace from=".C8 > .pin1" to={`net.${N5S}`} />
      <trace from=".C8 > .pin2" to={`net.${GND}`} />

      {/* ===== netlist: I mating header ===== */}
      <trace from=".H2 > .pin1" to={`net.${NVBUS}`} />
      <trace from=".H2 > .pin2" to={`net.${GND}`} />
      <trace from=".H2 > .pin3" to={`net.${N3V3}`} />
      <trace from=".H2 > .pin6" to={`net.${NSDA3}`} />
      <trace from=".H2 > .pin7" to={`net.${NSCL3}`} />

      {/* ===== netlist: D I2C translation ===== */}
      <trace from=".U2 > .VCCA" to={`net.${N3V3}`} />
      <trace from=".U2 > .VCCB" to={`net.${N5S}`} />
      <trace from=".U2 > .GND" to={`net.${GND}`} />
      <trace from=".U2 > .OE" to={`net.${N3V3}`} />
      <trace from=".U2 > .SDA_A" to={`net.${NSDA3}`} />
      <trace from=".U2 > .SCL_A" to={`net.${NSCL3}`} />
      <trace from=".U2 > .SDA_B" to={`net.${NSDA5}`} />
      <trace from=".U2 > .SCL_B" to={`net.${NSCL5}`} />
      <trace from=".R5 > .pin1" to={`net.${NSDA3}`} />
      <trace from=".R5 > .pin2" to={`net.${N3V3}`} />
      <trace from=".R6 > .pin1" to={`net.${NSCL3}`} />
      <trace from=".R6 > .pin2" to={`net.${N3V3}`} />
      <trace from=".R7 > .pin1" to={`net.${NSDA5}`} />
      <trace from=".R7 > .pin2" to={`net.${N5S}`} />
      <trace from=".R8 > .pin1" to={`net.${NSCL5}`} />
      <trace from=".R8 > .pin2" to={`net.${N5S}`} />

      {/* ===== netlist: E ADC + per-channel RC filters ===== */}
      <trace from=".U3 > .VDD" to={`net.${N5S}`} />
      <trace from=".U3 > .GND" to={`net.${GND}`} />
      <trace from=".U3 > .ADDR" to={`net.${GND}`} />
      <trace from=".U3 > .SDA" to={`net.${NSDA5}`} />
      <trace from=".U3 > .SCL" to={`net.${NSCL5}`} />
      <trace from=".U3 > .AIN0" to=".R9 > .pin2" />
      <trace from=".R9 > .pin1" to=".J2 > .pin3" />
      <trace from=".R9 > .pin2" to=".C11 > .pin1" />
      <trace from=".C11 > .pin2" to={`net.${GND}`} />
      <trace from=".R9 > .pin2" to=".D4 > .pin2" />
      <trace from=".D4 > .pin1" to={`net.${GND}`} />
      <trace from=".D4 > .pin3" to={`net.${N5S}`} />
      <trace from=".U3 > .AIN1" to=".R10 > .pin2" />
      <trace from=".R10 > .pin1" to=".J3 > .pin3" />
      <trace from=".R10 > .pin2" to=".C12 > .pin1" />
      <trace from=".C12 > .pin2" to={`net.${GND}`} />
      <trace from=".R10 > .pin2" to=".D5 > .pin2" />
      <trace from=".D5 > .pin1" to={`net.${GND}`} />
      <trace from=".D5 > .pin3" to={`net.${N5S}`} />
      <trace from=".U3 > .AIN2" to=".R11 > .pin2" />
      <trace from=".R11 > .pin1" to=".J4 > .pin3" />
      <trace from=".R11 > .pin2" to=".C13 > .pin1" />
      <trace from=".C13 > .pin2" to={`net.${GND}`} />
      <trace from=".R11 > .pin2" to=".D6 > .pin2" />
      <trace from=".D6 > .pin1" to={`net.${GND}`} />
      <trace from=".D6 > .pin3" to={`net.${N5S}`} />
      <trace from=".U3 > .AIN3" to=".R12 > .pin2" />
      <trace from=".R12 > .pin1" to=".J5 > .pin3" />
      <trace from=".R12 > .pin2" to=".C14 > .pin1" />
      <trace from=".C14 > .pin2" to={`net.${GND}`} />
      <trace from=".R12 > .pin2" to=".D7 > .pin2" />
      <trace from=".D7 > .pin1" to={`net.${GND}`} />
      <trace from=".D7 > .pin3" to={`net.${N5S}`} />
      <trace from=".C9 > .pin1" to={`net.${N5S}`} />
      <trace from=".C9 > .pin2" to={`net.${GND}`} />
      <trace from=".C10 > .pin1" to={`net.${N5S}`} />
      <trace from=".C10 > .pin2" to={`net.${GND}`} />

      {/* ===== netlist: F BMP280 ===== */}
      <trace from=".U4 > .VDD" to={`net.${N3V3}`} />
      <trace from=".U4 > .VDDIO" to={`net.${N3V3}`} />
      <trace from=".U4 > .GND1" to={`net.${GND}`} />
      <trace from=".U4 > .GND2" to={`net.${GND}`} />
      <trace from=".U4 > .SDI" to={`net.${NSDA3}`} />
      <trace from=".U4 > .SCK" to={`net.${NSCL3}`} />
      <trace from=".U4 > .SDO" to={`net.${GND}`} />
      <trace from=".U4 > .CSB" to={`net.${N3V3}`} />
      <trace from=".C15 > .pin1" to={`net.${N3V3}`} />
      <trace from=".C15 > .pin2" to={`net.${GND}`} />

      {/* ===== netlist: G RTC ===== */}
      <trace from=".U5 > .VCC" to={`net.${N3V3}`} />
      <trace from=".U5 > .GND" to={`net.${GND}`} />
      <trace from=".U5 > .SDA" to={`net.${NSDA3}`} />
      <trace from=".U5 > .SCL" to={`net.${NSCL3}`} />
      <trace from=".U5 > .VBAT" to=".BT1 > ._POS" />
      <trace from=".BT1 > ._NEG" to={`net.${GND}`} />
      <trace from=".C16 > .pin1" to={`net.${N3V3}`} />
      <trace from=".C16 > .pin2" to={`net.${GND}`} />
      {/* 32kHz / N_RST deliberately left open on the MZ */}

      {/* ===== netlist: H sensor connectors (pin1=+5V, pin2=GND, pin3=signal) ===== */}
      <trace from=".J2 > .pin1" to={`net.${N5S}`} />
      <trace from=".J2 > .pin2" to={`net.${GND}`} />
      <trace from=".J3 > .pin1" to={`net.${N5S}`} />
      <trace from=".J3 > .pin2" to={`net.${GND}`} />
      <trace from=".J4 > .pin1" to={`net.${N5S}`} />
      <trace from=".J4 > .pin2" to={`net.${GND}`} />
      <trace from=".J5 > .pin1" to={`net.${N5S}`} />
      <trace from=".J5 > .pin2" to={`net.${GND}`} />
    </board>
  )
}
