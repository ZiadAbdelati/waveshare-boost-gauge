import type { ChipProps } from "@tscircuit/props"

const pinLabels = {
  pin1: ["SDA_B"],
  pin2: ["GND"],
  pin3: ["VCCA"],
  pin4: ["SDA_A"],
  pin5: ["SCL_A"],
  pin6: ["OE"],
  pin7: ["VCCB"],
  pin8: ["SCL_B"]
} as const

const pinAttributes = {
  pin2: {requiresGround: true}
} as const

export const TCA9406DCTR = (props: ChipProps<typeof pinLabels>) => {
  return (
    <chip
      pinLabels={pinLabels}
      pinAttributes={pinAttributes}
      supplierPartNumbers={{
  "jlcpcb": [
    "C337496"
  ]
}}
      manufacturerPartNumber="TCA9406DCTR"
      footprint="dfn8_pillpads_p0.65mm_w4.85mm_pw0.4mm_pl1.2mm_pin1location(leftside,bottom)"
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C337496.obj?uuid=676f5e35950f4d7c8e9fec7736465692",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C337496.step?uuid=676f5e35950f4d7c8e9fec7736465692",
        pcbRotationOffset: 0,
        modelOriginPosition: { x: 0, y: 0, z: -0.6 },
      }}
      {...props}
    />
  )
}