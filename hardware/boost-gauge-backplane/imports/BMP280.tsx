import type { ChipProps } from "@tscircuit/props"

const pinLabels = {
  pin1: ["GND2"],
  pin2: ["CSB"],
  pin3: ["SDI"],
  pin4: ["SCK"],
  pin5: ["SDO"],
  pin6: ["VDDIO"],
  pin7: ["GND1"],
  pin8: ["VDD"]
} as const

const pinAttributes = {
  pin1: {requiresGround: true},
  pin7: {requiresGround: true},
  pin8: {requiresPower: true}
} as const

const footprinterPinLabels = {
  ...pinLabels,
  "pin8": [...pinLabels["pin8"], "pin1"],
  "pin7": [...pinLabels["pin7"], "pin2"],
  "pin6": [...pinLabels["pin6"], "pin3"],
  "pin5": [...pinLabels["pin5"], "pin4"],
  "pin4": [...pinLabels["pin4"], "pin5"],
  "pin3": [...pinLabels["pin3"], "pin6"],
  "pin2": [...pinLabels["pin2"], "pin7"],
  "pin1": [...pinLabels["pin1"], "pin8"],
} as const

export const BMP280 = (props: ChipProps<typeof pinLabels>) => {
  return (
    <chip
      pinLabels={footprinterPinLabels}
      pinAttributes={pinAttributes}
      supplierPartNumbers={{
  "jlcpcb": [
    "C83291"
  ]
}}
      manufacturerPartNumber="BMP280"
      footprint="lga8_grid4x0_p0.65mm_w2.05mm_pw0.4mm_pl0.6mm"
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C83291.obj?uuid=28cdaac5379d48238a3e4fc96f869007",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C83291.step?uuid=28cdaac5379d48238a3e4fc96f869007",
        pcbRotationOffset: 0,
        modelOriginPosition: { x: 0, y: -0.000012699999956566899, z: 0 },
      }}
      {...props}
    />
  )
}