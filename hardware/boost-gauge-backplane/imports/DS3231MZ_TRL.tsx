import type { ChipProps } from "@tscircuit/props"

const pinLabels = {
  pin1: ["32kHz"],
  pin2: ["VCC"],
  pin3: ["pin3"],
  pin4: ["N_RST"],
  pin5: ["GND"],
  pin6: ["VBAT"],
  pin7: ["SDA"],
  pin8: ["SCL"]
} as const

const pinAttributes = {
  pin2: {requiresPower: true},
  pin5: {requiresGround: true}
} as const

export const DS3231MZ_TRL = (props: ChipProps<typeof pinLabels>) => {
  return (
    <chip
      pinLabels={pinLabels}
      pinAttributes={pinAttributes}
      supplierPartNumbers={{
  "jlcpcb": [
    "C107410"
  ]
}}
      manufacturerPartNumber="DS3231MZ+TRL"
      footprint="soic8_pillpads_w7mm_pw0.59mm_pl1.8mm_pin1location(leftside,bottom)"
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C107410.obj?uuid=ec3b9f9b31a74655be3e55848dbee9c1",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C107410.step?uuid=ec3b9f9b31a74655be3e55848dbee9c1",
        pcbRotationOffset: 0,
        modelOriginPosition: { x: -0.000012700000070253736, y: 0, z: 0 },
      }}
      {...props}
    />
  )
}