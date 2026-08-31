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

export const LMR36520ADDAR = (props: ChipProps<typeof pinLabels>) => {
  return (
    <chip
      pinLabels={pinLabels}
      pinAttributes={pinAttributes}
      supplierPartNumbers={{
  "jlcpcb": [
    "C2879422"
  ]
}}
      manufacturerPartNumber="LMR36520ADDAR"
      footprint={<footprint>
        <smtpad portHints={["pin9"]} pcbX="0mm" pcbY="0mm" width="3.1999936mm" height="2.499995mm" shape="rect" />
        <smtpad portHints={["pin4"]} pcbX="1.905mm" pcbY="-2.999994mm" width="0.5999988mm" height="1.5999968mm" radius="0.2999994mm" shape="pill" />
        <smtpad portHints={["pin3"]} pcbX="0.635mm" pcbY="-2.999994mm" width="0.5999988mm" height="1.5999968mm" radius="0.2999994mm" shape="pill" />
        <smtpad portHints={["pin2"]} pcbX="-0.635mm" pcbY="-2.999994mm" width="0.5999988mm" height="1.5999968mm" radius="0.2999994mm" shape="pill" />
        <smtpad portHints={["pin5"]} pcbX="1.905mm" pcbY="2.999994mm" width="0.5999988mm" height="1.5999968mm" radius="0.2999994mm" shape="pill" />
        <smtpad portHints={["pin6"]} pcbX="0.635mm" pcbY="2.999994mm" width="0.5999988mm" height="1.5999968mm" radius="0.2999994mm" shape="pill" />
        <smtpad portHints={["pin7"]} pcbX="-0.635mm" pcbY="2.999994mm" width="0.5999988mm" height="1.5999968mm" radius="0.2999994mm" shape="pill" />
        <smtpad portHints={["pin1"]} pcbX="-1.905mm" pcbY="-2.999994mm" width="0.5999988mm" height="1.5999968mm" radius="0.2999994mm" shape="pill" />
        <smtpad portHints={["pin8"]} pcbX="-1.905mm" pcbY="2.999994mm" width="0.5999988mm" height="1.5999968mm" radius="0.2999994mm" shape="pill" />
        <courtyardoutline outline={[{x:-2.79,y:3.7552},{x:2.7392,y:3.7552},{x:2.7392,y:-3.7552},{x:-2.79,y:-3.7552},{x:-2.79,y:3.7552}]} />
      </footprint>}
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
