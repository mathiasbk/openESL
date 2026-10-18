#!/usr/bin/env python3
"""Release notes with the pull requests sorted by what they changed: AP firmware, tag
firmware, web page, or the rest (tools, docs, CI).

    release_notes.py [SINCE] [UNTIL]     markdown on stdout; SINCE is the previous release
                                         tag (default: the newest one), UNTIL a commit
                                         (default: HEAD)

Used by .github/workflows/release.yml for the notes of the GitHub release, and by hand for
the body of the release pull request (development -> master):

    python3 tools/release_notes.py v0.4.5 origin/development

Each pull request merged in the range (a merge commit "Merge pull request #N", or a squash
commit ending in "(#N)") is sorted by the files it changed, with the parts of
tools/fw_version.py: a pull request that touched both the AP and the tags is listed under
both. The AP and tag sections say the version the firmware carries, and "unchanged" when
no pull request touched that part since the previous release.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fw_version  # noqa: E402

SECTIONS = [("ap", "AP firmware"), ("tag", "Tag firmware"), ("web", "Web page"), ("other", "Tools, docs and CI")]


def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True, check=True).stdout


def pull_requests(since, until):
    """[(number, title, commit)] merged between since and until, newest first, without the
    release merges themselves (development -> master)"""
    fields = git("log", "--format=%H%x00%P%x00%s%x00%b%x00", "%s..%s" % (since, until)).split("\x00")
    prs = {}
    for i in range(0, len(fields) - 1, 4):
        sha, parents, subject, body = fields[i].strip(), fields[i + 1], fields[i + 2], fields[i + 3]
        m = re.match(r"Merge pull request #(\d+) from (\S+)", subject)
        if m:
            number, branch = int(m.group(1)), m.group(2)
            if branch.endswith("/development") or branch.endswith("/master"):
                continue  # the release merge
            title = body.strip().splitlines()[0] if body.strip() else branch
        else:
            m = re.search(r"\(#(\d+)\)$", subject)
            if not m or len(parents.split()) > 1:
                continue
            number, title = int(m.group(1)), subject[: m.start()].strip()
        prs.setdefault(number, (number, title, sha))
    return sorted(prs.values(), reverse=True)


def changed(sha, paths=()):
    """The files the pull request changed (the merge against its first parent), those the
    pathspecs select when given: git handles the excludes of fw_version.py and deleted files"""
    return git("diff-tree", "--no-commit-id", "--name-only", "-r", "-m", "--first-parent", sha, "--", *paths).split()


def sort(prs, parts):
    sections = {k: [] for k, _ in SECTIONS}
    for pr in prs:
        files = changed(pr[2])
        where = [k for k in ("ap", "tag") if changed(pr[2], parts[k])]
        if any(f.startswith("web/") for f in files):
            where.append("web")
        # the rest: tools, CI, docs; tests and READMEs of the firmware go with their part
        if not where or any(not f.startswith(("web/", "ap/", "tag/", "shared/", "docs/")) and not f.endswith(".md")
                            for f in files):
            where.append("other")
        for k in where:
            sections[k].append(pr)
    return sections


def notes(since, until, versions):
    parts = fw_version.parts()
    prs = pull_requests(since, until)
    sections = sort(prs, parts)
    lines = []
    for key, name in SECTIONS:
        head = name
        if key in versions:
            head += " %s" % versions[key]
            if not sections[key]:
                head += " (unchanged since %s: nothing new to flash)" % since
        lines.append("## " + head)
        lines.append("")
        for number, title, _ in sections[key] or []:
            lines.append("- %s (#%d)" % (title, number))
        if not sections[key]:
            lines.append("- no changes")
        lines.append("")
    if not prs:
        lines.append("No pull requests between %s and %s." % (since, until))
    return "\n".join(lines).rstrip() + "\n"


def main():
    args = sys.argv[1:]
    if args[:1] in (["-h"], ["--help"]):
        sys.exit(__doc__)
    tags = fw_version.release_tags() or []
    since = args[0] if args else (tags[0] if tags else None)
    until = args[1] if len(args) > 1 else "HEAD"
    if not since:
        sys.exit("release_notes.py: no release tag to start from")
    versions = {part: fw_version.version(part) for part in ("ap", "tag")}
    sys.stdout.write(notes(since, until, versions))


if __name__ == "__main__":
    main()
