/**
 * @file Text and JSON output helpers for the rack CLI.
 *
 * @brief Line-oriented output seam plus the renderers every command shares: compact
 *        single-line JSON for machines (--json) and aligned tables / pretty JSON for humans.
 *
 * @note Commands never touch the console directly: everything flows through OutputPorts so
 *       tests can capture stdout/stderr without spawning a process.
 */

import type { InstanceInfo, JsonValue } from '../../src/core/types.ts';
import { formatVersion } from '../../src/core/version.ts';

/**
 * @brief Line-oriented output ports the CLI writes to.
 *
 * @param line One already-formatted line without a trailing newline.
 * @note Injection seam: the real entry wires console.log/console.error, tests capture arrays.
 */
export interface OutputPorts {
  out(line: string): void;
  err(line: string): void;
}

/**
 * @brief Extract a human-readable message from any thrown value.
 *
 * @param value Anything caught in a catch clause.
 * @return The Error message, or String(value) for foreign throwables.
 */
export function errorMessage(value: unknown): string {
  return value instanceof Error ? value.message : String(value);
}

/**
 * @brief Coerce a CLI-internal value into a plain JSON data-plane value.
 *
 * @param value Runtime value expected to be JSON-serializable (interface snapshots, call
 *        results).
 * @return A structurally clean JsonValue: prototypes reset, undefined members dropped.
 * @note Uses a JSON round trip both for the type cast and as a hard serializability check;
 *       interfaces like InstanceInfo carry no index signature and need this bridge.
 */
export function asJsonValue(value: unknown): JsonValue {
  return JSON.parse(JSON.stringify(value)) as JsonValue;
}

/**
 * @brief Emit one machine-readable single-line JSON document.
 *
 * @param ports Output ports to write to.
 * @param value Document to serialize.
 */
export function emitJson(ports: OutputPorts, value: JsonValue): void {
  ports.out(JSON.stringify(value));
}

/**
 * @brief Emit a human-readable pretty-printed JSON document.
 *
 * @param ports Output ports to write to.
 * @param value Document to serialize.
 */
export function emitPrettyJson(ports: OutputPorts, value: JsonValue): void {
  for (const line of JSON.stringify(value, null, 2).split('\n')) {
    ports.out(line);
  }
}

/** @brief One column of a rendered text table. */
interface Column<Row> {
  readonly header: string;
  readonly render: (row: Row) => string;
}

/**
 * @brief Render rows as an aligned, two-space separated text table.
 *
 * @param rows Data rows in output order; may be empty.
 * @param columns Column definitions in output order.
 * @return One string per output line, header first.
 */
function renderTable<Row>(rows: readonly Row[], columns: readonly Column<Row>[]): string[] {
  const widths: number[] = columns.map((column) =>
    Math.max(column.header.length, ...rows.map((row) => column.render(row).length)),
  );
  const lines: string[] = [
    columns
      .map((column, index) => column.header.padEnd(widths[index] ?? 0))
      .join('  ')
      .trimEnd(),
  ];
  for (const row of rows) {
    lines.push(
      columns
        .map((column, index) => column.render(row).padEnd(widths[index] ?? 0))
        .join('  ')
        .trimEnd(),
    );
  }
  return lines;
}

/**
 * @brief Render the plugin table shown by `list`: plugId / version / state / generation.
 *
 * @param instances Snapshot returned by Rack.plugins().
 * @return One string per output line, header first.
 */
export function renderInstanceTable(instances: readonly InstanceInfo[]): string[] {
  return renderTable(instances, [
    { header: 'PLUG ID', render: (info) => info.plugId },
    { header: 'VERSION', render: (info) => formatVersion(info.version) },
    { header: 'STATE', render: (info) => info.state },
    { header: 'GENERATION', render: (info) => String(info.generation) },
  ]);
}
