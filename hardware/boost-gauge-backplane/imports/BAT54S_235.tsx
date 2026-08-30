import type { ChipProps } from "@tscircuit/props"

const pinLabels = {
  pin1: ["A"],
  pin2: ["C"],
  pin3: ["pin3"]
} as const

export const BAT54S_235 = (props: ChipProps<typeof pinLabels>) => {
  return (
    <chip
      pinLabels={pinLabels}
      symbol={
        <symbol>
          <schematicpath points={[{"x":-0.2,"y":0.1},{"x":-0.2,"y":0.3},{"x":0.2,"y":0.3},{"x":0.2,"y":0.1}]} strokeColor="#880000" />
          <schematicpath svgPath="M -0.08 -0.1 L -0.2 0.1 L -0.34 -0.1 Z" strokeColor="#880000" isFilled fillColor="#880000" />
          <schematicpath points={[{"x":-0.08,"y":0.16},{"x":-0.06,"y":0.16},{"x":-0.06,"y":0.12},{"x":-0.36,"y":0.12},{"x":-0.36,"y":0.08},{"x":-0.34,"y":0.08}]} strokeColor="#880000" />
          <port name="pin2" pinNumber={2} aliases={["C"]} direction="down" schX={0.2} schY={-0.4} schStemLength={0.3} />
          <schematicpath svgPath="M 0.08 0.1 L 0.2 -0.1 L 0.34 0.1 Z" strokeColor="#880000" isFilled fillColor="#880000" />
          <schematicpath points={[{"x":0.08,"y":-0.16},{"x":0.06,"y":-0.16},{"x":0.06,"y":-0.12},{"x":0.36,"y":-0.12},{"x":0.36,"y":-0.08},{"x":0.34,"y":-0.08}]} strokeColor="#880000" />
          <port name="pin3" pinNumber={3} aliases={["3"]} direction="up" schX={0} schY={0.6} schStemLength={0.3} />
          <port name="pin1" pinNumber={1} aliases={["A"]} direction="down" schX={-0.2} schY={-0.4} schStemLength={0.3} />
        </symbol>
      }
      supplierPartNumbers={{
  "jlcpcb": [
    "C503463"
  ]
}}
      manufacturerPartNumber="BAT54S,235"
      footprint="sot23w_p1mm_pw0.65mm_pin1location(rightside,bottom)"
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C503463.obj?uuid=d777607a152f4f3aac9bb0d0c14ed6fd",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C503463.step?uuid=d777607a152f4f3aac9bb0d0c14ed6fd",
        pcbRotationOffset: 180,
        modelOriginPosition: { x: 0.000012700000070253736, y: -0.000012699999956566899, z: 0.050795 },
      }}
      {...props}
    />
  )
}