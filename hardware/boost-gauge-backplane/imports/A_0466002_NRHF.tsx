import type { ChipProps } from "@tscircuit/props"

const pinLabels = {
  pin1: ["pin1"],
  pin2: ["pin2"]
} as const

export const A_0466002_NRHF = (props: ChipProps<typeof pinLabels>) => {
  return (
    <chip
      pinLabels={pinLabels}
      symbol={
        <symbol>
          <schematicpath points={[{"x":-0.2,"y":0},{"x":0.2,"y":0}]} strokeColor="#8D2323" />
          <port name="pin2" pinNumber={2} aliases={["2"]} direction="right" schX={0.4} schY={0} schStemLength={0.2} />
          <port name="pin1" pinNumber={1} aliases={["1"]} direction="left" schX={-0.4} schY={0} schStemLength={0.2} />
          <schematicrect schX={0} schY={0} width={0.52} height={0.12} color="#880000" />
        </symbol>
      }
      supplierPartNumbers={{
  "jlcpcb": [
    "C3105"
  ]
}}
      manufacturerPartNumber="0466002.NRHF"
      footprint="smdpads2_p2.89mm_pw1.1901mm_ph1.728mm"
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C3105.obj?uuid=7dbd95a5ee9a45949b72cb8147e267ff",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C3105.step?uuid=7dbd95a5ee9a45949b72cb8147e267ff",
        pcbRotationOffset: 0,
        modelOriginPosition: { x: 0, y: -0.000012699999999199463, z: 0 },
      }}
      {...props}
    />
  )
}