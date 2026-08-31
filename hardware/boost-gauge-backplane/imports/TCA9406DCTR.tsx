import type { ChipProps } from "@tscircuit/props"

const pinLabels = {
  pin1: ["SDA_B"],
  pin2: ["GND"],
  pin3: ["VCCA"],
  pin4: ["SDA_A"],
  pin5: ["SCL_A"],
  pin6: ["OE"],
  pin7: ["VCCB"],
  pin8: ["SCL_B"]
} as const

const pinAttributes = {
  pin2: {requiresGround: true}
} as const

export const TCA9406DCTR = (props: ChipProps<typeof pinLabels>) => {
  return (
    <chip
      pinLabels={pinLabels}
      pinAttributes={pinAttributes}
      supplierPartNumbers={{
  "jlcpcb": [
    "C337496"
  ]
}}
      manufacturerPartNumber="TCA9406DCTR"
      footprint={<footprint>
        <smtpad portHints={["pin1"]} pcbX="-0.975106mm" pcbY="-1.82499mm" width="0.3999992mm" height="1.1999976mm" radius="0.1999996mm" shape="pill" />
        <smtpad portHints={["pin2"]} pcbX="-0.32512mm" pcbY="-1.82499mm" width="0.3999992mm" height="1.1999976mm" radius="0.1999996mm" shape="pill" />
        <smtpad portHints={["pin3"]} pcbX="0.32512mm" pcbY="-1.82499mm" width="0.3999992mm" height="1.1999976mm" radius="0.1999996mm" shape="pill" />
        <smtpad portHints={["pin4"]} pcbX="0.975106mm" pcbY="-1.82499mm" width="0.3999992mm" height="1.1999976mm" radius="0.1999996mm" shape="pill" />
        <smtpad portHints={["pin5"]} pcbX="0.975106mm" pcbY="1.82499mm" width="0.3999992mm" height="1.1999976mm" radius="0.1999996mm" shape="pill" />
        <smtpad portHints={["pin6"]} pcbX="0.32512mm" pcbY="1.82499mm" width="0.3999992mm" height="1.1999976mm" radius="0.1999996mm" shape="pill" />
        <smtpad portHints={["pin7"]} pcbX="-0.32512mm" pcbY="1.82499mm" width="0.3999992mm" height="1.1999976mm" radius="0.1999996mm" shape="pill" />
        <smtpad portHints={["pin8"]} pcbX="-0.975106mm" pcbY="1.82499mm" width="0.3999992mm" height="1.1999976mm" radius="0.1999996mm" shape="pill" />
        <silkscreenpath route={[{x:-1.499997,y:0.999998},{x:1.499997,y:0.999998},{x:1.499997,y:-0.999998},{x:-1.499997,y:-0.999998}]} />
        <silkscreenpath route={[{x:-1.524,y:0.254},{x:-1.524,y:0.999998}]} />
        <silkscreenpath route={[{x:-1.524,y:-0.254},{x:-1.524,y:-0.999998}]} />
        <silkscreenpath route={[{x:-1.524,y:-0.254},{x:-1.703605,y:-0.179605},{x:-1.778,y:0},{x:-1.703605,y:0.179605},{x:-1.524,y:0.254}]} />
        <silkscreentext text="{NAME}" pcbX="-0.022606mm" pcbY="3.21361mm" anchorAlignment="center" fontSize="1mm" />
        <courtyardoutline outline={[{x:-1.783906,y:2.46361},{x:1.738694,y:2.46361},{x:1.738694,y:-2.53219},{x:-1.783906,y:-2.53219},{x:-1.783906,y:2.46361}]} />
      </footprint>}
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C337496.obj?uuid=676f5e35950f4d7c8e9fec7736465692",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C337496.step?uuid=676f5e35950f4d7c8e9fec7736465692",
        pcbRotationOffset: 0,
        modelOriginPosition: { x: 0, y: 0, z: -0.6 },
      }}
      {...props}
    />
  )
}
