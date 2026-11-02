# Run inside RenderDoc: qrenderdoc.exe --python tools/rdoc/rdc_info.py
# Reads build/rdoc/job.txt (capture path, output dir) and writes, for the
# capture of Xenia's D3D12 work: draws.txt (every draw: event id, name,
# output targets and their sizes/formats) and the final colour output of
# every render target written (rt_<eventid>_<resource>.png), i.e. Xenia's
# ground truth per pass.
import os
import traceback

ROOT = r"F:\FromDustRecomp\build\rdoc"
LOG = os.path.join(ROOT, "script.log")


def main():
    import renderdoc as rd
    capture, out = open(os.path.join(ROOT, "job.txt")).read().split("\n")[:2]
    os.makedirs(out, exist_ok=True)
    cap = rd.OpenCaptureFile()
    cap.OpenFile(capture, "", None)
    result = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = result[1] if isinstance(result, tuple) else result
    textures = {t.resourceId: t for t in controller.GetTextures()}
    actions = []

    def walk(items):
        for a in items:
            actions.append(a)
            walk(a.children)

    walk(controller.GetRootActions())
    draws = [a for a in actions if a.flags & rd.ActionFlags.Drawcall]
    last_write = {}
    with open(os.path.join(out, "draws.txt"), "w") as f:
        for a in draws:
            outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
            desc = []
            for o in outs:
                t = textures.get(o)
                desc.append("%s:%dx%d:%s" % (int(o), t.width, t.height, t.format.Name()) if t else str(int(o)))
                last_write[o] = a.eventId
            f.write("%d %s %s\n" % (a.eventId, a.GetName(controller.GetStructuredFile()), " ".join(desc)))
    # Every render target's content after the last draw writing it.
    for resource, event in sorted(last_write.items(), key=lambda kv: kv[1]):
        controller.SetFrameEvent(event, True)
        save = rd.TextureSave()
        save.resourceId = resource
        save.destType = rd.FileType.PNG
        save.alpha = rd.AlphaMapping.Preserve
        controller.SaveTexture(save, os.path.join(out, "rt_%05d_%d.png" % (event, int(resource))))
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
    open(LOG, "w").write("ok\n")
except Exception:
    open(LOG, "w").write(traceback.format_exc())
os._exit(0)
