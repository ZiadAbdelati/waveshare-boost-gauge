import type { ChipProps } from "@tscircuit/props"

const pinLabels = { pin1: ["pin1"], pin2: ["pin2"] } as const

export const SMBJ16CA = (props: ChipProps<typeof pinLabels>) => (
  <chip
    pinLabels={pinLabels}
    supplierPartNumbers={{ jlcpcb: ["C353385"] }}
    manufacturerPartNumber="SMBJ16CA"
    footprint={
      <footprint>
        <smtpad portHints={["pin2"]} pcbX="2.591308mm" pcbY="0mm" width="2.047494mm" height="2.2409912mm" shape="rect" />
        <smtpad portHints={["pin1"]} pcbX="-2.591308mm" pcbY="0mm" width="2.047494mm" height="2.2409912mm" shape="rect" />
        <courtyardoutline outline={[{ x: -3.8568, y: 2.155 }, { x: 3.8822, y: 2.155 }, { x: 3.8822, y: -2.155 }, { x: -3.8568, y: -2.155 }, { x: -3.8568, y: 2.155 }]} />
      </footprint>
    }
    cadModel={{
      objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C353385.obj?uuid=e41aaff7d3944047a9ed53ff7c0cd6e4",
      stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C353385.step?uuid=e41aaff7d3944047a9ed53ff7c0cd6e4",
      pcbRotationOffset: 0,
      modelOriginPosition: { x: 0, y: 0.0000127, z: -1.26 },
    }}
    {...props}
  />
)
