import argparse
import json
import re
import sys
import os
from typing import List
from dataclasses import dataclass


@dataclass
class MetricInfo:
    scope: str
    name: str
    unit: str
    source_file: str
    source_line: int

    def __post_init__(self):
        if self.scope == "":
            self.scope = f"{self.source_file}:{self.source_line}"

    @property
    def key(self) -> str:
        return f"{self.scope}::{self.name}[{self.unit}]" if self.unit else f"{self.scope}::{self.name}"


def extract_metrics(filepath: str) -> List[MetricInfo]:
    with open(filepath, 'r') as f:
        data = json.load(f)

    metrics = []
    metric_regex = r'@METRIC\(([^:]*?)::([^[\]]+?)(?:\[([^\]]*)\])?=([^)]+)\)'

    for entry in data.get("StringConstants", []):
        if len(entry) >= 2:
            string_content = entry[1]
            file_match = re.match(
                r'\("([^"]+)", (\d+), \d+, \{\}, """[^"]*"""\)(.*)', string_content)
            if not file_match:
                continue

            filename = file_match.group(1)
            line_num = int(file_match.group(2))
            message = file_match.group(3)

            for match in re.finditer(metric_regex, message):
                scope = match.group(1) if match.group(1) else ""
                name = match.group(2)
                unit = match.group(3) if match.group(3) else ""

                metrics.append(MetricInfo(
                    scope, name, unit, filename, line_num))

    seen_keys = set()
    unique_metrics = []
    for metric in metrics:
        if metric.key not in seen_keys:
            seen_keys.add(metric.key)
            unique_metrics.append(metric)

    return sorted(unique_metrics, key=lambda m: m.key)


def generate_ssproj(metrics: List[MetricInfo], title: str) -> str:
    js_parser = '''const METRIC_INDEX_MAP = {
'''

    for i, metric in enumerate(metrics):
        js_parser += f'    "{metric.key}": {i + 1},\n'

    js_parser += f'''}};

let parsedValues = new Array({len(metrics) + 1}).fill(0);

function parse(frame) {{
    if (frame.length > 0) {{
        try {{
            let data = JSON.parse(frame);
            if (data.hasOwnProperty('time')) {{
                parsedValues[0] = parseFloat(data.time) || 0;
            }}
            if (data.name && data.scope && data.hasOwnProperty('value')) {{
                const unit = data.unit || "";
                const key = unit ? `${{data.scope}}::${{data.name}}[${{unit}}]` : `${{data.scope}}::${{data.name}}`;
                if (METRIC_INDEX_MAP.hasOwnProperty(key)) {{
                    parsedValues[METRIC_INDEX_MAP[key]] = parseFloat(data.value) || 0;
                }}
            }}
        }} catch (e) {{}}
    }}
    return parsedValues.slice();
}}'''

    datasets = []

    datasets.append({
        "graph": False,
        "index": 1,
        "title": "Time",
        "units": "s",
        "xAxis": -1
    })

    for i, metric in enumerate(metrics):
        datasets.append({
            "graph": True,
            "index": i + 2,
            "title": f"{metric.scope}::{metric.name}",
            "units": metric.unit,
            "xAxis": 1
        })

    project = {
        "frameStart": "/*",
        "frameEnd": "*/",
        "frameParser": js_parser,
        "groups": [
            {
                "datasets": datasets,
                "title": f"{title}"
            }
        ],
        "title": f"{title} Project"
    }

    return json.dumps(project, indent=4)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        prog='generate_ssproj',
        description='create ssproj file from string constants json')

    parser.add_argument('--input', help='input json file', required=True)
    parser.add_argument('--output', help='output ssproj file', required=True)

    args = parser.parse_args()

    # Validate required arguments
    if not args.input:
        print("Error: --input is required", file=sys.stderr)
        sys.exit(1)
    if not args.output:
        print("Error: --output is required", file=sys.stderr)
        sys.exit(1)

    if not os.path.exists(args.input):
        print(f"Error: Input file '{args.input}' not found.", file=sys.stderr)
        sys.exit(1)

    # Determine title from input filename
    filename = os.path.basename(args.input)
    if filename.endswith("_string_constants.json"):
        title = filename.replace("_string_constants.json", "")
    else:
        title = os.path.splitext(filename)[0]

    try:
        metrics = extract_metrics(args.input)
        ssproj_content = generate_ssproj(metrics, title)

        with open(args.output, 'w') as outfile:
            outfile.write(ssproj_content)
    except IOError as e:
        print(
            f"Error writing output file '{args.output}': {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(
            f"Error processing input file '{args.input}': {e}", file=sys.stderr)
        sys.exit(1)
