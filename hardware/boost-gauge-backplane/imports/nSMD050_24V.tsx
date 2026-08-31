import type { ChipProps } from "@tscircuit/props"

const pinLabels = { pin1: ["pin1"], pin2: ["pin2"] } as const

export const NSMD050_24V = (props: ChipProps<typeof pinLabels>) => (
  <chip
    pinLabels={pinLabels}
    supplierPartNumbers={{ jlcpcb: ["C70076"] }}
    manufacturerPartNumber="nSMD050-24V"
    footprint={
      <footprint>
        <smtpad portHints={["pin1"]} pcbX="-1.445006mm" pcbY="0mm" width="1.1900916mm" height="1.7279874mm" shape="rect" />
        <smtpad portHints={["pin2"]} pcbX="1.445006mm" pcbY="0mm" width="1.1900916mm" height="1.7279874mm" shape="rect" />
        <courtyardoutline outline={[{ x: -2.5106, y: 1.3422 }, { x: 2.536, y: 1.3422 }, { x: 2.536, y: -1.3422 }, { x: -2.5106, y: -1.3422 }, { x: -2.5106, y: 1.3422 }]} />
      </footprint>
    }
    cadModel={{
      objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C70076.obj?uuid=7dbd95a5ee9a45949b72cb8147e267ff",
      stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C70076.step?uuid=7dbd95a5ee9a45949b72cb8147e267ff",
      pcbRotationOffset: 0,
      modelOriginPosition: { x: 0, y: -0.0000127, z: 0 },
    }}
    {...props}
  />
)
