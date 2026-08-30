import type { DiodeProps } from "@tscircuit/props"

const pinLabels = {
  pin1: ["anode","pos"],
  pin2: ["cathode","neg"]
} as const

export const SS54 = (props: DiodeProps) => {
  const { name = "D1", ...restProps } = props

  return (
    <diode
      name={name}
      pinLabels={pinLabels}
      supplierPartNumbers={{
  "jlcpcb": [
    "C22452"
  ]
}}
      manufacturerPartNumber="SS54"
      footprint="smdpads2_p5.0074mm_pw2.0625mm_ph1.539mm_pin1location(rightside,top)"
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C22452.obj?uuid=16f5f0e72b3e4f43a77ecac0b819c7f8",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C22452.step?uuid=16f5f0e72b3e4f43a77ecac0b819c7f8",
        pcbRotationOffset: 0,
        modelOriginPosition: { x: 0, y: 0, z: -0.1 },
      }}
      {...restProps}
    />
  )
}