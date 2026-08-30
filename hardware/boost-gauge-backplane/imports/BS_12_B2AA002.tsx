import type { ChipProps } from "@tscircuit/props"

const pinLabels = {
  pin1: ["_POS"],
  pin2: ["_NEG"]
} as const

export const BS_12_B2AA002 = (props: ChipProps<typeof pinLabels>) => {
  return (
    <chip
      pinLabels={pinLabels}
      symbol={
        <symbol>
          <schematicpath points={[{"x":-0.1,"y":0.3},{"x":-0.1,"y":-0.3}]} strokeColor="#8D2323" />
          <schematicpath points={[{"x":0,"y":0.1},{"x":0,"y":-0.1}]} strokeColor="#8D2323" />
          <schematicpath points={[{"x":0.1,"y":0.3},{"x":0.1,"y":-0.3}]} strokeColor="#8D2323" />
          <schematicpath points={[{"x":0.2,"y":0.1},{"x":0.2,"y":-0.1}]} strokeColor="#8D2323" />
          <schematicpath points={[{"x":-0.2,"y":0},{"x":-0.12,"y":0}]} strokeColor="#8D2323" />
          <port name="pin2" pinNumber={2} aliases={["_NEG"]} direction="right" schX={0.4} schY={0} schStemLength={0.2} />
          <port name="pin1" pinNumber={1} aliases={["_POS"]} direction="left" schX={-0.4} schY={0} schStemLength={0.2} />
        </symbol>
      }
      supplierPartNumbers={{
  "jlcpcb": [
    "C964721"
  ]
}}
      manufacturerPartNumber="BS-12-B2AA002"
      footprint="smdpads2_p16.5999mm_pw4.6mm_ph2.8mm_pin1location(rightside,top)"
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C964721.obj?uuid=091370a67c4744a2bbaddffc4f4b20a7",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C964721.step?uuid=091370a67c4744a2bbaddffc4f4b20a7",
        pcbRotationOffset: 0,
        modelOriginPosition: { x: 0, y: -0.000012700000070253736, z: -0.7 },
      }}
      {...props}
    />
  )
}