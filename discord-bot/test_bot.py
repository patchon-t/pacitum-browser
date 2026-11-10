"""Offline checks for the Pacitum bot (no Discord token or network needed): python test_bot.py"""
import asyncio
import os
import pathlib
import tempfile
from types import SimpleNamespace as NS

os.environ.setdefault("STATE_DIR", tempfile.mkdtemp())
import discord  # noqa: E402
import pacitum_bot as b  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent
NOTES = HERE.parent / "release-notes" / "v2.0.5.md"
ASSETS = ["PacitumBrowser-2.0.5-linux-x86_64.AppImage", "PacitumBrowser-2.0.5-linux-x86_64.AppImage.sha256",
          "PacitumBrowser-2.0.5-linux-x86_64.deb", "PacitumBrowser-2.0.5-linux-x86_64.rpm",
          "PacitumBrowser-Setup-2.0.5.exe", "PacitumBrowser-Setup-2.0.5.exe.sha256"]
RELEASE = {"tag_name": "v2.0.5", "name": "Pacitum Browser 2.0.5",
           "body": NOTES.read_text(encoding="utf-8").replace("\n", "\r\n") if NOTES.exists() else "# x\n\nNotes",
           "html_url": "https://github.com/patchon-t/pacitum-browser/releases/tag/v2.0.5",
           "assets": [{"name": n, "browser_download_url": "https://example.invalid/" + n} for n in ASSETS]}


def test_commands():
    assert sorted(c.name for c in b.bot.tree.get_commands()) == ["bug", "download", "post-release", "release", "suggest"]


async def test_forms():   # modals need a running event loop
    assert len(b.BugForm().children) == 5   # Discord allows at most 5 fields
    assert len(b.SuggestForm().children) == 3


def test_split():
    parts = b.split_message(("## Part\n" + "x" * 80 + "\n") * 80)
    assert len(parts) > 1 and all(len(p) <= 1900 for p in parts)
    assert all(len(p) <= 1900 for p in b.split_message("y" * 5000))


def test_release_messages():
    msgs = b.release_messages(RELEASE)
    assert msgs[0].startswith("# 🚀 Pacitum Browser 2.0.5\n")
    assert "\r" not in "".join(msgs) and all(len(m) <= 2000 for m in msgs)
    assert msgs[0].count("Pacitum Browser 2.0.5") == 1   # the notes' own heading is dropped


async def test_download_buttons():
    labels = [i.label for i in b.download_view(RELEASE).children]
    assert labels == ["Windows", "Linux .deb", "Linux .rpm", "AppImage", "Release page"], labels


async def test_send_release_ping_once():
    sent = []

    class Channel:
        async def send(self, content, **kw):
            sent.append((content, kw))

    b.RELEASE_PING_ROLE_ID = 42
    await b.send_release(Channel(), RELEASE, ping=True)
    assert sent[0][0].startswith("<@&42>") and not any(c.startswith("<@&") for c, _ in sent[1:])
    assert "view" in sent[-1][1] and all("view" not in kw for _, kw in sent[:-1])


async def test_status_buttons():
    embed = discord.Embed(title="🐞 x")
    embed.add_field(name="Version", value="2.0.5")
    embed.add_field(name="Status", value="🆕 New")
    out = {}

    async def edit_message(embed):
        out["embed"] = embed

    async def send_message(text, ephemeral=False):
        out["refused"] = text

    async def thread_send(text, allowed_mentions=None):
        out["thread"] = text

    def interaction(can_manage):
        return NS(type=discord.InteractionType.component, data={"custom_id": "pacitum:bug:fixed"},
                  user=NS(guild_permissions=NS(manage_messages=can_manage), display_name="Team", mention="@Team"),
                  message=NS(embeds=[embed], thread=NS(send=thread_send)),
                  response=NS(edit_message=edit_message, send_message=send_message))

    await b.PacitumBot.on_interaction(b.bot, interaction(False))
    assert "refused" in out and "embed" not in out
    out.clear()
    await b.PacitumBot.on_interaction(b.bot, interaction(True))
    assert out["embed"].fields[1].value.startswith("✅ Fixed")
    assert out["embed"].color.value == 0x2EB67D and "Fixed" in out["thread"]


def test_no_mentions():
    out = b.no_mentions("thanks @octocat and @team-x, mail a@b.c")
    assert "@octocat" not in out and "@team-x" not in out and "@\u200boctocat" in out


def test_clean_version():
    assert b.clean_version("2.0.5") == "2.0.5" and b.clean_version(" v2.1.0-beta.1 ") == "v2.1.0-beta.1"
    assert b.clean_version(None) is None
    for bad in ("../../x", "2.0/../", "latest", "2.0.5?x=1", ""):
        try:
            b.clean_version(bad)
        except ValueError:
            continue
        raise AssertionError(bad)


def test_rate_limit():
    r = b.RateLimit(2)
    assert r.allow("a", 0) and r.allow("a", 1) and not r.allow("a", 2) and r.allow("b", 2) and r.allow("a", 3700)


async def test_crash_relay():
    from aiohttp import FormData
    from aiohttp.test_utils import TestClient, TestServer
    got = []

    async def send(content, files):
        got.append((content, files))

    async with TestClient(TestServer(b.crash_app(send, b.RateLimit(2)))) as client:
        form = FormData()
        form.add_field("payload_json", '{"content": "**Crash report** @everyone"}', content_type="application/json")
        form.add_field("files[0]", b"log line", filename="crash.log", content_type="text/plain")
        r = await client.post("/crash", data=form)
        assert r.status == 204, r.status
        assert got[0][0].startswith("**Crash report**") and got[0][1] == [("crash.log", b"log line")]
        assert (await client.post("/crash", data=b"x")).status == 400
        assert (await client.post("/crash", data=b"x")).status == 429   # 2 per hour in this test


async def test_already_posted():
    me = NS(id=1)

    class Channel:
        def history(self, limit):
            async def gen():
                yield NS(author=NS(id=2), content="# 🚀 Pacitum Browser 2.0.5")
                yield NS(author=me, content="# 🚀 Pacitum Browser 2.0.5\nnotes")
            return gen()

    assert await b.already_posted(Channel(), RELEASE, me)
    assert not await b.already_posted(Channel(), {"name": "Pacitum Browser 2.0.6"}, me)


def test_state():
    b.write_state({"last_release": "v2.0.5"})
    assert b.read_state() == {"last_release": "v2.0.5"}


def main():
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith("test_") and callable(f)]
    for name, fn in tests:
        result = fn()
        if asyncio.iscoroutine(result):
            asyncio.run(result)
        print("ok", name)
    print(f"{len(tests)} tests passed")


if __name__ == "__main__":
    main()
