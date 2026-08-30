import type { ChipProps } from "@tscircuit/props"

const pinLabels = {
  pin1: ["pin1"],
  pin2: ["pin2"]
} as const

export const BSMD1206_200_12V = (props: ChipProps<typeof pinLabels>) => {
  return (
    <chip
      pinLabels={pinLabels}
      symbol={
        <symbol>
          <port name="pin2" pinNumber={2} aliases={["2"]} direction="right" schX={0.6} schY={0} schStemLength={0.2} />
          <port name="pin1" pinNumber={1} aliases={["1"]} direction="left" schX={-0.4} schY={0} schStemLength={0.2} />
          <schematicpath points={[{"x":0.3,"y":0},{"x":0.4,"y":0}]} strokeColor="#880000" />
          <schematicpath svgPath="M -0.1 0 C 0 0.2 0.1 0 0.1 0 C 0.2 -0.2 0.3 0 0.3 0" strokeColor="#880000" />
          <schematicpath points={[{"x":-0.2,"y":0},{"x":-0.1,"y":0}]} strokeColor="#880000" />
        </symbol>
      }
      supplierPartNumbers={{
  "jlcpcb": [
    "C883135"
  ]
}}
      manufacturerPartNumber="BSMD1206-200-12V"
      footprint="smdpads2_p2.89mm_pw1.1901mm_ph1.728mm"
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C883135.obj?uuid=7dbd95a5ee9a45949b72cb8147e267ff",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C883135.step?uuid=7dbd95a5ee9a45949b72cb8147e267ff",
        pcbRotationOffset: 0,
        modelOriginPosition: { x: 0, y: -0.000012699999999199463, z: 0 },
      }}
      {...props}
    />
  )
}