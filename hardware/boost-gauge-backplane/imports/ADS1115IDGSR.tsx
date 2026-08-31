import type { ChipProps } from "@tscircuit/props"

const pinLabels = {
  pin1: ["ADDR"],
  pin2: ["pin2"],
  pin3: ["GND"],
  pin4: ["AIN0"],
  pin5: ["AIN1"],
  pin6: ["AIN2"],
  pin7: ["AIN3"],
  pin8: ["VDD"],
  pin9: ["SDA"],
  pin10: ["SCL"]
} as const

const pinAttributes = {
  pin3: {requiresGround: true},
  pin8: {requiresPower: true}
} as const

export const ADS1115IDGSR = (props: ChipProps<typeof pinLabels>) => {
  return (
    <chip
      pinLabels={pinLabels}
      pinAttributes={pinAttributes}
      supplierPartNumbers={{
  "jlcpcb": [
    "C37593"
  ]
}}
      manufacturerPartNumber="ADS1115IDGSR"
      footprint={<footprint>
        <smtpad portHints={["pin1"]} pcbX="-0.999998mm" pcbY="-2.109978mm" width="0.2800096mm" height="1.620012mm" radius="0.1400048mm" shape="pill" />
        <smtpad portHints={["pin2"]} pcbX="-0.499872mm" pcbY="-2.109978mm" width="0.2800096mm" height="1.620012mm" radius="0.1400048mm" shape="pill" />
        <smtpad portHints={["pin3"]} pcbX="0mm" pcbY="-2.109978mm" width="0.2800096mm" height="1.620012mm" radius="0.1400048mm" shape="pill" />
        <smtpad portHints={["pin4"]} pcbX="0.500126mm" pcbY="-2.109978mm" width="0.2800096mm" height="1.620012mm" radius="0.1400048mm" shape="pill" />
        <smtpad portHints={["pin5"]} pcbX="0.999998mm" pcbY="-2.109978mm" width="0.2800096mm" height="1.620012mm" radius="0.1400048mm" shape="pill" />
        <smtpad portHints={["pin10"]} pcbX="-0.999998mm" pcbY="2.109978mm" width="0.2800096mm" height="1.620012mm" radius="0.1400048mm" shape="pill" />
        <smtpad portHints={["pin9"]} pcbX="-0.499872mm" pcbY="2.109978mm" width="0.2800096mm" height="1.620012mm" radius="0.1400048mm" shape="pill" />
        <smtpad portHints={["pin8"]} pcbX="0mm" pcbY="2.109978mm" width="0.2800096mm" height="1.620012mm" radius="0.1400048mm" shape="pill" />
        <smtpad portHints={["pin7"]} pcbX="0.500126mm" pcbY="2.109978mm" width="0.2800096mm" height="1.620012mm" radius="0.1400048mm" shape="pill" />
        <smtpad portHints={["pin6"]} pcbX="0.999998mm" pcbY="2.109978mm" width="0.2800096mm" height="1.620012mm" radius="0.1400048mm" shape="pill" />
        <silkscreenpath route={[{x:-1.576197,y:-1.0713974},{x:-1.576197,y:1.0713974},{x:1.576197,y:1.0713974},{x:1.576197,y:-1.0713974},{x:-1.576197,y:-1.0713974}]} />
        <silkscreencircle pcbX="-0.999998mm" pcbY="-0.319024mm" radius="0.150114mm" />
        <silkscreencircle pcbX="-1.592326mm" pcbY="-2.109978mm" radius="0.150114mm" />
        <silkscreentext text="{NAME}" pcbX="-0.0889mm" pcbY="3.7686mm" anchorAlignment="center" fontSize="1mm" />
        <courtyardoutline outline={[{x:-2.0026,y:3.0186},{x:1.8248,y:3.0186},{x:1.8248,y:-3.1456},{x:-2.0026,y:-3.1456},{x:-2.0026,y:3.0186}]} />
      </footprint>}
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C37593.obj?uuid=a8ab40c7a7c54773a59b712bf86c7131",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C37593.step?uuid=a8ab40c7a7c54773a59b712bf86c7131",
        pcbRotationOffset: 0,
        modelOriginPosition: { x: 0, y: 0, z: 0 },
      }}
      {...props}
    />
  )
}
