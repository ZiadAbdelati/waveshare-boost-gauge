import type { ChipProps } from "@tscircuit/props"

const pinLabels = { pin1: ["G"], pin2: ["S"], pin3: ["D"] } as const

export const DMP4065S_7 = (props: ChipProps<typeof pinLabels>) => (
  <chip
    pinLabels={pinLabels}
    supplierPartNumbers={{ jlcpcb: ["C182476"] }}
    manufacturerPartNumber="DMP4065S-7"
    footprint={
      <footprint>
        <smtpad portHints={["pin1"]} pcbX="0.999998mm" pcbY="-0.94996mm" width="0.999998mm" height="0.6500114mm" shape="rect" />
        <smtpad portHints={["pin2"]} pcbX="0.999998mm" pcbY="0.94996mm" width="0.999998mm" height="0.6500114mm" shape="rect" />
        <smtpad portHints={["pin3"]} pcbX="-0.999998mm" pcbY="0mm" width="0.999998mm" height="0.6500114mm" shape="rect" />
        <courtyardoutline outline={[{ x: -1.7486, y: 1.774 }, { x: 1.7994, y: 1.774 }, { x: 1.7994, y: -1.774 }, { x: -1.7486, y: -1.774 }, { x: -1.7486, y: 1.774 }]} />
      </footprint>
    }
    cadModel={{
      objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C182476.obj?uuid=d777607a152f4f3aac9bb0d0c14ed6fd",
      stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C182476.step?uuid=d777607a152f4f3aac9bb0d0c14ed6fd",
      pcbRotationOffset: 180,
      modelOriginPosition: { x: 0.0000127, y: -0.0000127, z: 0.050795 },
    }}
    {...props}
  />
)
