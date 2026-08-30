import type { InductorProps } from "@tscircuit/props"

export const FXL0530_4R7_M = (props: Omit<InductorProps, "inductance">) => {
  return (
    <inductor
      inductance="4.7uH"
      supplierPartNumbers={{
  "jlcpcb": [
    "C177246"
  ]
}}
      manufacturerPartNumber="FXL0530-4R7-M"
      footprint="smdpads2_p4.1001mm_pw1.9mm_ph2.5mm"
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C177246.obj?uuid=2bfbf5dfddf249e49a8eec094f024758",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C177246.step?uuid=2bfbf5dfddf249e49a8eec094f024758",
        pcbRotationOffset: 0,
        modelOriginPosition: { x: 0, y: 0, z: -0.01 },
      }}
      {...props}
    />
  )
}