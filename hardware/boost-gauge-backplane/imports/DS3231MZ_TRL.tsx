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
      footprint={<footprint>
        <smtpad portHints={["pin5"]} pcbX="1.905mm" pcbY="2.599944mm" width="0.58801mm" height="1.7999964mm" radius="0.294005mm" shape="pill" />
        <smtpad portHints={["pin6"]} pcbX="0.635mm" pcbY="2.599944mm" width="0.58801mm" height="1.7999964mm" radius="0.294005mm" shape="pill" />
        <smtpad portHints={["pin7"]} pcbX="-0.635mm" pcbY="2.599944mm" width="0.58801mm" height="1.7999964mm" radius="0.294005mm" shape="pill" />
        <smtpad portHints={["pin8"]} pcbX="-1.905mm" pcbY="2.599944mm" width="0.58801mm" height="1.7999964mm" radius="0.294005mm" shape="pill" />
        <smtpad portHints={["pin4"]} pcbX="1.905mm" pcbY="-2.599944mm" width="0.58801mm" height="1.7999964mm" radius="0.294005mm" shape="pill" />
        <smtpad portHints={["pin3"]} pcbX="0.635mm" pcbY="-2.599944mm" width="0.58801mm" height="1.7999964mm" radius="0.294005mm" shape="pill" />
        <smtpad portHints={["pin2"]} pcbX="-0.635mm" pcbY="-2.599944mm" width="0.58801mm" height="1.7999964mm" radius="0.294005mm" shape="pill" />
        <smtpad portHints={["pin1"]} pcbX="-1.905mm" pcbY="-2.599944mm" width="0.58801mm" height="1.7999964mm" radius="0.294005mm" shape="pill" />
        <silkscreenpath route={[{x:-2.526208,y:-1.5214092},{x:2.526208,y:-1.5214092},{x:2.526208,y:1.5214092},{x:-2.526208,y:1.5214092},{x:-2.526208,y:-1.5214092}]} />
        <silkscreencircle pcbX="-1.905mm" pcbY="-1.016mm" radius="0.150114mm" />
        <silkscreentext text="{NAME}" pcbX="0.0127mm" pcbY="4.2004mm" anchorAlignment="center" fontSize="1mm" />
        <courtyardoutline outline={[{x:-2.7646,y:3.4504},{x:2.79,y:3.4504},{x:2.79,y:-3.806},{x:-2.7646,y:-3.806},{x:-2.7646,y:3.4504}]} />
      </footprint>}
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
