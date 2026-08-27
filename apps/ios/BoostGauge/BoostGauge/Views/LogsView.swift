import SwiftUI

struct LogsView: View {
    @EnvironmentObject var session: AppSession
    @StateObject private var vm = LogsViewModel()

    @State private var showShare = false
    @State private var shareURL: URL?

    var body: some View {
        NavigationStack {
            Group {
                if vm.samples.isEmpty && !vm.isLoading {
                    VStack(spacing: 8) {
                        Image(systemName: "tray")
                            .font(.largeTitle)
                            .foregroundColor(.secondary)
                        Text("No log samples yet")
                            .foregroundColor(.secondary)
                        if let error = vm.errorMessage {
                            Text(error)
                                .font(.footnote)
                                .foregroundColor(.orange)
                        }
                        Button("Load logs") {
                            Task { await vm.load() }
                        }
                    }
                } else {
                    List {
                        if let error = vm.errorMessage {
                            Text(error)
                                .font(.footnote)
                                .foregroundColor(.orange)
                        }
                        Section("Pressure history") {
                            HStack(spacing: 8) {
                                ForEach(LogWindow.all) { window in
                                    Button(action: { Task { await vm.load(limit: window.limit) } }) {
                                        Text(window.title)
                                            .font(.caption.weight(.semibold))
                                            .padding(.horizontal, 12)
                                            .padding(.vertical, 5)
                                            .background(
                                                Capsule().fill(vm.window == window ? Color.cyan.opacity(0.22) : Color(.tertiarySystemFill))
                                            )
                                            .foregroundColor(vm.window == window ? Color.cyan : Color.secondary)
                                    }
                                    .buttonStyle(.plain)
                                    .disabled(vm.isLoading)
                                    .accessibilityIdentifier("logsWindow.\(window.limit)")
                                }
                                Spacer()
                                Text(vm.scopeLabel)
                                    .font(.caption.monospaced())
                                    .lineLimit(1)
                                    .minimumScaleFactor(0.75)
                                    .foregroundColor(.secondary)
                                    .accessibilityIdentifier("logsSampleCount")
                            }
                            LogPressureChart(samples: vm.samples, anchor: vm.anchor, revision: vm.dataRevision,
                                 fixtureCrosshair: ProcessInfo.processInfo.arguments.contains("-e2eCrosshair"))
                                .frame(height: 200)
                                .padding(.vertical, 8)
                            if let minimum = vm.samples.map(\.psi).min(),
                               let maximum = vm.samples.map(\.psi).max() {
                                HStack {
                                    Label("Min \(Format.psi2(minimum)) psi", systemImage: "arrow.down")
                                    Spacer()
                                    Label("Max \(Format.psi2(maximum)) psi", systemImage: "arrow.up")
                                }
                                .font(.caption.monospacedDigit())
                                .foregroundColor(.secondary)
                            }
                        }
                    }
                }
            }
.gaugeScrollBottomMargin()
                        .navigationTitle("Logs")
            .toolbar {
                ToolbarItemGroup(placement: .navigationBarTrailing) {
                    Button(action: { Task { await vm.load(limit: vm.window.limit) } }) {
                        Image(systemName: "arrow.clockwise")
                    }
                    Button(action: exportCSV) {
                        Image(systemName: "square.and.arrow.up")
                    }
                    .disabled(vm.samples.isEmpty)
                }
            }
            .sheet(isPresented: $showShare) {
                if let shareURL {
                    ActivityView(items: [shareURL])
                }
            }
            .onAppear {
                let history: GaugeTransport?
                if session.transport?.transportKind == "BLE", let host = session.effectiveHTTPHost,
                   let url = URL(string: host), url.scheme != nil {
                    history = HttpTransport(baseURL: url)
                } else {
                    history = nil
                }
                vm.reset(transport: session.transport, historyTransport: history)
            }
            .onChange(of: session.transportID) { _, _ in
                let history: GaugeTransport?
                if session.transport?.transportKind == "BLE", let host = session.effectiveHTTPHost,
                   let url = URL(string: host), url.scheme != nil {
                    history = HttpTransport(baseURL: url)
                } else {
                    history = nil
                }
                vm.reset(transport: session.transport, historyTransport: history)
                Task { await vm.load() }
            }
            .task { await vm.load() }
        }
    }

    private func exportCSV() {
        let csv = vm.csvText()
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("boost-gauge-log.csv")
        do {
            try csv.write(to: url, atomically: true, encoding: .utf8)
            shareURL = url
            showShare = true
        } catch {
            vm.errorMessage = error.localizedDescription
        }
    }
}

struct LogPressureChart: View {
    let samples: [LogSample]
    var anchor: GaugeState?
    /// The LogsViewModel's data revision: the downsampled-column cache key so
    /// a refreshed window with the same column count still rebuilds its columns.
    var revision: Int = 0
    /// Test/screenshot fixture only: when true and no finger is down, show the
    /// crosshair readout at the newest sample so the UI is deterministic to
    /// capture. Gated by the `-e2eCrosshair` launch argument; never on in real
    /// use. Release behavior is unchanged — a real touch still owns the state.
    var fixtureCrosshair: Bool = false

    private let yAxisWidth: CGFloat = 38
    private let xAxisHeight: CGFloat = 20
    private let topInset: CGFloat = 10
    private let rightInset: CGFloat = 10

    @State private var crosshair: Crosshair?
    @State private var userTouched = false
    /// Holds the downsampled display columns (one psi per column) plus the psi
    /// values and domain extrema. Rebuilt only when (revision, column count)
    /// changes; a crosshair drag re-renders the body but reads this cache
    /// instead of re-running `samples.map(\.psi)` + `series` every frame.
    @State private var columnCache = ColumnCache()

    private struct Crosshair {
        let x: CGFloat
        let y: CGFloat
        let value: Double
        let sampleIndex: Int
    }

    /// Reference cache (mutating a class held by @State never reassigns the
    /// state itself, so SwiftUI does not flag it as a body-time mutation).
    final class ColumnCache {
        struct Key: Equatable {
            let revision: Int
            let columns: Int
        }

        private var key: Key?
        private(set) var values: [Double] = []
        private(set) var series: [Double] = []
        private(set) var minimum: Double = 0
        private(set) var maximum: Double = 1
        /// Incremented on every actual rebuild; a crosshair drag must not
        /// change it (the cache is the per-frame read path).
        private(set) var rebuildCount = 0

        func update(revision: Int, samples: [LogSample], columns: Int) {
            let newKey = Key(revision: revision, columns: columns)
            guard newKey != key else { return }
            key = newKey
            rebuildCount += 1
            let started = Date()
            values = samples.map(\.psi)
            series = LogPressureChart.series(values, columns: columns)
            minimum = min(values.min() ?? 0, 0)
            maximum = max(values.max() ?? 1, 0)
            NSLog("[Logs] chart columns rebuilt samples=%d columns=%d downsampled=%.1fms",
                  samples.count, columns, Date().timeIntervalSince(started) * 1000)
        }
    }

    var body: some View {
        GeometryReader { proxy in
            let plot = Self.plotRect(size: proxy.size, yAxisWidth: yAxisWidth,
                                     xAxisHeight: xAxisHeight, topInset: topInset, rightInset: rightInset)
            let columns = max(Int(plot.width.rounded()), 1)
            let _ = columnCache.update(revision: revision, samples: samples, columns: columns)
            // ONE series: a single representative psi value per display column
            // (the column's last sample). The old min/max envelope stroked
            // BOTH the per-column maxima and minima as separate cyan lines,
            // which render as two distinct squiggles on a dense sweep.
            let values = columnCache.values
            let series = columnCache.series
            let minimum = columnCache.minimum
            let maximum = columnCache.maximum
            let domain = Self.chartDomain(minimum: minimum, maximum: maximum)
            let span = max(domain.max - domain.min, 1)
            // A real finger owns the crosshair; the -e2eCrosshair fixture only
            // fills in before the first touch, so a real release still clears it.
            let fixturePoint = !userTouched && fixtureCrosshair ? series.last.map { last in
                Crosshair(
                    x: plot.maxX,
                    y: plot.minY + plot.height * CGFloat((domain.max - last) / span),
                    value: last,
                    sampleIndex: Self.sampleIndex(column: max(series.count - 1, 0), count: values.count, columns: columns)
                )
            } : nil
            let effectiveCrosshair = crosshair ?? fixturePoint

            ZStack(alignment: .topLeading) {
                plotCard
                gridLines(plot: plot, domain: domain)
                zeroLine(plot: plot, domain: domain)
                dataLine(plot: plot, series: series, domain: domain)
                xAxisLabels(plot: plot, count: values.count)
                crosshairOverlay(plot: plot, point: effectiveCrosshair)
            }
            .contentShape(Rectangle())
            .clipShape(RoundedRectangle(cornerRadius: 10))
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        userTouched = true
                        guard plot.width > 0, columns > 0, !values.isEmpty, !series.isEmpty, !samples.isEmpty else { return }
                        let x = min(max(value.location.x, plot.minX), plot.maxX)
                        let rawColumn = Int((x - plot.minX) / plot.width * CGFloat(columns))
                        let column = min(max(rawColumn, 0), columns - 1)
                        guard series.indices.contains(column) else { return }
                        let psiValue = series[column]
                        let y = plot.minY + plot.height * CGFloat((domain.max - psiValue) / span)
                        let rawIndex = Self.sampleIndex(column: column, count: values.count, columns: columns)
                        // Clamp every computed index into samples.indices before subscripting.
                        guard !samples.isEmpty else { return }
                        let clampedIndex = min(max(rawIndex, samples.indices.lowerBound), samples.indices.upperBound)
                        guard samples.indices.contains(clampedIndex) else { return }
                        crosshair = Crosshair(
                            x: x,
                            y: y,
                            value: psiValue,
                            sampleIndex: clampedIndex
                        )
                    }
                    .onEnded { _ in crosshair = nil }
            )
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Pressure history graph with \(samples.count) samples")
        // Invalidate gesture-derived state when data identity changes so a stale
        // crosshair index from a previous sample count cannot outlive the data.
        .onChange(of: samples) { _, _ in
            crosshair = nil
            userTouched = false
        }
    }

    private var plotCard: some View {
        RoundedRectangle(cornerRadius: 10).fill(Color(.tertiarySystemFill))
    }

    private func gridLines(plot: CGRect, domain: (min: Double, max: Double, step: Double)) -> some View {
        let span = max(domain.max - domain.min, 1)
        return ForEach(Self.psiTicks(domain: domain), id: \.self) { tick in
            let y = plot.minY + plot.height * CGFloat((domain.max - tick) / span)
            Path { path in
                path.move(to: CGPoint(x: plot.minX, y: y))
                path.addLine(to: CGPoint(x: plot.maxX, y: y))
            }
            .stroke(Color.secondary.opacity(0.15), lineWidth: 1)
            Text(Self.tickLabel(tick))
                .font(.caption2.monospacedDigit())
                .foregroundColor(.secondary)
                .frame(width: yAxisWidth - 6, alignment: .trailing)
                .position(x: plot.minX - (yAxisWidth - 6) / 2, y: y)
        }
    }

    private func zeroLine(plot: CGRect, domain: (min: Double, max: Double, step: Double)) -> some View {
        let span = max(domain.max - domain.min, 1)
        let zeroY = plot.minY + plot.height * CGFloat((domain.max - 0) / span)
        return Path { path in
            path.move(to: CGPoint(x: plot.minX, y: zeroY))
            path.addLine(to: CGPoint(x: plot.maxX, y: zeroY))
        }
        .stroke(Color.secondary.opacity(0.45), style: StrokeStyle(lineWidth: 1, dash: [4, 4]))
    }

    private func dataLine(plot: CGRect, series: [Double], domain: (min: Double, max: Double, step: Double)) -> some View {
        Path { path in
            Self.trace(series, width: plot.width, height: plot.height, origin: plot.origin, domain: domain, into: &path)
        }
        .stroke(Color.cyan, style: StrokeStyle(lineWidth: 2, lineJoin: .round))
    }

    private func xAxisLabels(plot: CGRect, count: Int) -> some View {
        let fractions: [CGFloat] = [0, 0.25, 0.5, 0.75, 1]
        let lo = min(plot.minX + 30, plot.midX)
        let hi = max(plot.maxX - 30, plot.midX)
        // Drop any label that would collide with the one before it. Five
        // "HH:mm" ticks only fit when the plot is wide enough; on a narrow
        // portrait plot showing every tick made adjacent labels overlap.
        var kept: [(fraction: CGFloat, x: CGFloat)] = []
        for fraction in fractions {
            let rawX = plot.minX + plot.width * fraction
            let x = min(max(rawX, lo), hi)
            if let last = kept.last, x - last.x < 46 { continue }
            kept.append((fraction, x))
        }
        return ForEach(Array(kept.enumerated()), id: \.offset) { _, entry in
            let sampleIndex = min(Int(entry.fraction * CGFloat(max(count - 1, 0))), max(count - 1, 0))
            Text(axisTime(for: sampleIndex))
                .font(.caption2.monospacedDigit())
                .foregroundColor(.secondary)
                .position(x: entry.x, y: plot.maxY + xAxisHeight / 2)
        }
    }

    @ViewBuilder
    private func crosshairOverlay(plot: CGRect, point: Crosshair?) -> some View {
        if let point {
            Path { path in
                path.move(to: CGPoint(x: point.x, y: plot.minY))
                path.addLine(to: CGPoint(x: point.x, y: plot.maxY))
            }
            .stroke(Color.cyan.opacity(0.6), style: StrokeStyle(lineWidth: 1, dash: [3, 3]))

            Circle()
                .fill(Color.cyan)
                .frame(width: 8, height: 8)
                .position(x: point.x, y: point.y)

            let text = "\(Format.psi(point.value)) psi · \(timeText(for: point.sampleIndex))"
            Text(text)
                .font(.caption2.monospacedDigit())
                .foregroundColor(.white)
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(Capsule().fill(Color.cyan))
                .position(x: plot.midX, y: plot.minY + 4)
                .accessibilityIdentifier("logsChartCrosshair")
        }
    }

    private func timeText(for index: Int) -> String {
        guard !samples.isEmpty else { return "" }
        let clamped = min(max(index, samples.indices.lowerBound), samples.indices.upperBound)
        guard samples.indices.contains(clamped) else { return "" }
        let sample = samples[clamped]
        if let epoch = sampleEpochMs(sample) {
            return Format.time(epoch, offsetMinutes: anchor?.timezoneOffsetMinutes)
        }
        return Self.relativeTime(sample.tMs, newestMs: samples.last?.tMs)
    }

    /// Compact axis tick ("HH:mm") so five labels fit across a narrow portrait
    /// plot without colliding; the crosshair pill keeps the full seconds form.
    private func axisTime(for index: Int) -> String {
        guard !samples.isEmpty else { return "" }
        let clamped = min(max(index, samples.indices.lowerBound), samples.indices.upperBound)
        guard samples.indices.contains(clamped) else { return "" }
        let sample = samples[clamped]
        if let epoch = sampleEpochMs(sample) {
            return String(Format.time(epoch, offsetMinutes: anchor?.timezoneOffsetMinutes).prefix(5))
        }
        return Self.relativeTime(sample.tMs, newestMs: samples.last?.tMs)
    }

    private func sampleEpochMs(_ sample: LogSample) -> Int64? {
        guard let anchor,
              let anchorEpoch = anchor.epochMs,
              let anchorUptime = anchor.uptimeMs,
              let tMs = sample.tMs else { return nil }
        return anchorEpoch - Int64(anchorUptime) + tMs
    }

    /// Reduces a dense ring to ONE value per display column: the column's last
    /// sample. A single series keeps the line exactly on the real samples (the
    /// graph extremes match the raw min/max caption) without the envelope
    /// bandwidth that made a fast sweep render as two parallel lines.
    static func series(_ values: [Double], columns: Int) -> [Double] {
        guard !values.isEmpty else { return [] }
        if values.count == 1 {
            return Array(repeating: values[0], count: columns)
        }
        return (0..<columns).map { column in
            let start = column * values.count / columns
            let end = max(start + 1, (column + 1) * values.count / columns)
            let slice = values[start..<min(end, values.count)]
            return slice.last ?? 0
        }
    }

    /// Which raw sample produced a given display column (the column's last).
    static func sampleIndex(column: Int, count: Int, columns: Int) -> Int {
        guard count > 0, columns > 0 else { return 0 }
        let clampedColumn = min(max(column, 0), columns - 1)
        let start = clampedColumn * count / columns
        let end = max(start + 1, (clampedColumn + 1) * count / columns)
        return min(max(end - 1, 0), count - 1)
    }

    /// Expands the raw data extent to "nice" round bounds at a readable grid
    /// step so gridlines and labels land on clean psi values.
    static func chartDomain(minimum: Double, maximum: Double) -> (min: Double, max: Double, step: Double) {
        let rawStep = (maximum - minimum) / 5
        let step = niceStep(rawStep)
        let domainMin = (minimum / step).rounded(.down) * step
        let domainMax = (maximum / step).rounded(.up) * step
        return (domainMin, domainMax, step)
    }

    static func niceStep(_ raw: Double) -> Double {
        guard raw > 0, raw.isFinite else { return 1 }
        let exponent = floor(log10(raw))
        let fraction = raw / pow(10, exponent)
        let nice: Double
        if fraction <= 1 { nice = 1 }
        else if fraction <= 2 { nice = 2 }
        else if fraction <= 5 { nice = 5 }
        else { nice = 10 }
        return nice * pow(10, exponent)
    }

    static func psiTicks(domain: (min: Double, max: Double, step: Double)) -> [Double] {
        var ticks: [Double] = []
        var value = domain.min
        while value <= domain.max + 1e-9 {
            ticks.append(value)
            value += domain.step
        }
        return ticks
    }

    static func tickLabel(_ value: Double) -> String {
        if value == 0 { return "0" }
        if value == value.rounded() { return String(format: "%.0f", value) }
        return String(format: "%.1f", value)
    }

    static func relativeTime(_ tMs: Int64?, newestMs: Int64?) -> String {
        guard let tMs, let newestMs else { return "" }
        let elapsed = newestMs - tMs
        guard elapsed >= 0 else { return "now" }
        if elapsed < 2_000 { return "now" }
        let seconds = elapsed / 1000
        return "-\(seconds / 60):\(String(format: "%02d", seconds % 60))"
    }

    static func plotRect(size: CGSize, yAxisWidth: CGFloat, xAxisHeight: CGFloat,
                         topInset: CGFloat, rightInset: CGFloat) -> CGRect {
        CGRect(
            x: yAxisWidth,
            y: topInset,
            width: max(size.width - yAxisWidth - rightInset, 1),
            height: max(size.height - topInset - xAxisHeight, 1)
        )
    }

    static func trace(_ ys: [Double], width: CGFloat, height: CGFloat, origin: CGPoint,
                      domain: (min: Double, max: Double, step: Double), into path: inout Path) {
        let span = max(domain.max - domain.min, 1)
        for (index, value) in ys.enumerated() {
            let x = origin.x + width * CGFloat(index) / CGFloat(max(ys.count - 1, 1))
            let y = origin.y + height * CGFloat((domain.max - value) / span)
            if index == 0 { path.move(to: CGPoint(x: x, y: y)) }
            else { path.addLine(to: CGPoint(x: x, y: y)) }
        }
    }
}
