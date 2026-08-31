import type { InductorProps } from "@tscircuit/props"

export const FXL0530_100_M = (props: Omit<InductorProps, "inductance">) => {
  return (
    <inductor
      inductance="10uH"
      supplierPartNumbers={{
  "jlcpcb": [
    "C177248"
  ]
}}
      manufacturerPartNumber="FXL0530-100-M"
      footprint="smdpads2_p4.1001mm_pw1.9mm_ph2.5mm"
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C177248.obj?uuid=2bfbf5dfddf249e49a8eec094f024758",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C177248.step?uuid=2bfbf5dfddf249e49a8eec094f024758",
        pcbRotationOffset: 0,
        modelOriginPosition: { x: 0, y: 0, z: -0.01 },
      }}
      {...props}
    />
  )
}