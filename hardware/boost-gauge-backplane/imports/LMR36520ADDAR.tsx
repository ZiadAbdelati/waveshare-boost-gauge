import type { ChipProps } from "@tscircuit/props"

const pinLabels = {
  pin1: ["PGND"],
  pin2: ["VIN"],
  pin3: ["EN"],
  pin4: ["PG"],
  pin5: ["FB"],
  pin6: ["VCC"],
  pin7: ["BOOT"],
  pin8: ["SW"],
  pin9: ["EP"]
} as const

const pinAttributes = {
  pin1: {requiresGround: true},
  pin2: {requiresPower: true},
  pin6: {requiresPower: true}
} as const

const footprinterPinLabels = {
  ...pinLabels,
  "pin9": [...pinLabels["pin9"], "thermalpad"],
} as const

export const LMR36520ADDAR = (props: ChipProps<typeof pinLabels>) => {
  return (
    <chip
      pinLabels={footprinterPinLabels}
      pinAttributes={pinAttributes}
      supplierPartNumbers={{
  "jlcpcb": [
    "C2879422"
  ]
}}
      manufacturerPartNumber="LMR36520ADDAR"
      footprint="soic8_thermalpad2.5mmx3.2mm_pillpads_w7.6mm_pl1.6mm_pin1location(leftside,bottom)"
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C2879422.obj?uuid=8a93c3c8e269400f8c283f37d8055e89",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C2879422.step?uuid=8a93c3c8e269400f8c283f37d8055e89",
        pcbRotationOffset: 0,
        modelOriginPosition: { x: 0.000012700000070253736, y: 0, z: -0.91 },
      }}
      {...props}
    />
  )
}