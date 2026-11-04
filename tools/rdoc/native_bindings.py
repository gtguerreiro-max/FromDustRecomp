"""Inspect RenderDoc's XML export without opening its GUI.

renderdoccmd convert -f replay.rdc -o replay.xml -c zip.xml
python tools/rdoc/native_bindings.py replay.xml [--json bindings.json]
"""
import argparse
import collections
import copy
import json
import xml.etree.ElementTree as ET


def field(node, name):
    return node.find("*[@name='%s']" % name)


def number(node, name):
    value = field(node, name)
    return int(value.text) if value is not None else None


def inspect(path):
    states, draws, counts = {}, [], collections.Counter()
    for _, chunk in ET.iterparse(path, events=("end",)):
        if chunk.tag != "chunk":
            continue
        name = chunk.get("name", "")
        counts[name] += 1
        command_list = number(chunk, "pCommandList")
        state = states.setdefault(command_list, {"constants": {}})
        if name.endswith("::SetGraphicsRootConstantBufferView"):
            buffer = field(chunk, "BufferLocation")
            state["constants"][number(chunk, "RootParameterIndex")] = {
                "buffer": number(buffer, "Buffer"), "offset": number(buffer, "Offset")}
        elif name.endswith("::SetPipelineState"):
            state["pipeline"] = number(chunk, "pPipelineState")
        elif name.endswith("::IASetIndexBuffer"):
            view = field(chunk, "pView")
            if view is not None:
                location = field(view, "BufferLocation")
                fmt = field(view, "Format")
                state["index"] = {
                    "buffer": number(location, "Buffer"), "offset": number(location, "Offset"),
                    "format": fmt.get("string") if fmt is not None else None}
        elif name.endswith("::DrawIndexedInstanced"):
            draw = copy.deepcopy(state)
            draw.update(chunk=int(chunk.get("chunkIndex")), command_list=command_list,
                        count=number(chunk, "IndexCountPerInstance"))
            draws.append(draw)
        chunk.clear()
    bad_alignment = []
    for draw in draws:
        index = draw.get("index", {})
        size = 4 if index.get("format") == "DXGI_FORMAT_R32_UINT" else 2
        if index.get("offset", 0) % size:
            bad_alignment.append(draw["chunk"])
    return {"draw_count": len(draws), "misaligned_index_chunks": bad_alignment,
            "draws": draws, "calls": dict(counts)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xml")
    parser.add_argument("--json")
    args = parser.parse_args()
    result = inspect(args.xml)
    if args.json:
        with open(args.json, "w") as output:
            json.dump(result, output, indent=2)
    print("%d indexed draws; misaligned index views: %s" %
          (result["draw_count"], result["misaligned_index_chunks"]))
    for draw in result["draws"][-4:]:
        print(json.dumps(draw))


if __name__ == "__main__":
    main()
