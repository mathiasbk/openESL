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
both. The AP and tag sections say the version the firmware carries at UNTIL (the tag section
each model's, when they differ), and "unchanged" when no pull request touched that part since
the previous release.

The notes start with a summary of a few sentences, the section "### Summary of X.Y.Z" of
docs/release.md. The changes come next, then the upgrade instructions: the first paragraph of the section
"### Upgrading from A to X.Y.Z" of docs/release.md for the version in VERSION at UNTIL (the short
version: the order and what to watch out for), with a link to the whole section. Every release
must have both, so the script stops with an error when one is missing, and

    release_notes.py --check             both sections for VERSION are in docs/release.md (the
                                         files as they are here; release.yml runs it first,
                                         and test_release_notes.py checks it in CI)
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fw_version  # noqa: E402

REPO = os.environ.get("GITHUB_REPOSITORY", "mathiasbk/openESL")
RELEASE_MD = "docs/release.md"
UPGRADE_HEAD = re.compile(r"^### Upgrading from (.+) to (\S+)\s*$")
SUMMARY_HEAD = re.compile(r"^### Summary of (\S+)\s*$")

SECTIONS = [("ap", "AP firmware"), ("tag", "Tag firmware"), ("web", "Web page"), ("other", "Tools, docs and CI")]


def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True, check=True).stdout


def upgrade_section(text, version, pattern=UPGRADE_HEAD):
    """(heading, body) of the section "### Upgrading from A to <version>" (or the one pattern
    matches, its last group the version) in the text of docs/release.md, or None"""
    lines = text.splitlines()
    for i, head in enumerate(lines):
        m = pattern.match(head)
        if m and m.group(m.lastindex) == version:
            body = []
            for line in lines[i + 1:]:
                if line.startswith("#"):
                    break
                body.append(line)
            return head.lstrip("#").strip(), "\n".join(body).strip()
    return None


def absolute_links(body, ref):
    """The links of docs/release.md made to point at the repository at ref, since the release
    page on GitHub does not resolve links relative to docs/"""
    def fix(m):
        target, anchor = (m.group(1).split("#", 1) + [""])[:2]
        path = os.path.normpath(os.path.join(os.path.dirname(RELEASE_MD), target)).replace(os.sep, "/")
        return "](https://github.com/%s/blob/%s/%s%s)" % (REPO, ref, path, "#" + anchor if anchor else "")
    return re.sub(r"\]\((?!https?:|#|mailto:)([^)\s]+)\)", fix, body)


def anchor(heading):
    """GitHub's anchor for a heading: "Upgrading from 0.6.x to 0.7.0" -> upgrading-from-06x-to-070"""
    return re.sub(r"[^a-z0-9 -]", "", heading.lower()).replace(" ", "-")


def upgrade_notes(text, version, ref):
    """The upgrade instructions at the end of the notes: the first paragraph of the section and a
    link to the rest, or SystemExit when docs/release.md has none for this version"""
    found = upgrade_section(text, version)
    if not found:
        sys.exit("release_notes.py: %s has no section \"### Upgrading from <previous> to %s\". Every release "
                 "needs upgrade instructions (docs/release.md, \"Upgrade instructions\")." % (RELEASE_MD, version))
    heading, body = found
    summary = body.split("\n\n", 1)[0].strip()
    url = "https://github.com/%s/blob/%s/%s" % (REPO, ref, RELEASE_MD)
    return "\n".join([
        "## " + heading, "",
        absolute_links(summary, ref), "",
        "All details, and what going back costs: [docs/release.md](%s#%s). From an older release: "
        "[upgrading over more than one release](%s#upgrading-over-more-than-one-release)." % (url, anchor(heading), url), "",
    ])


def summary(text, version, ref):
    """The summary at the top of the notes, or SystemExit when docs/release.md has none for this
    version"""
    found = upgrade_section(text, version, SUMMARY_HEAD)
    if not found or not found[1]:
        sys.exit("release_notes.py: %s has no section \"### Summary of %s\". Every release needs a summary of "
                 "a few sentences (docs/release.md, \"Summaries\")." % (RELEASE_MD, version))
    return absolute_links(found[1], ref) + "\n"


def pull_requests(since, until):
    """[(number, title, commit)] merged between since and until, newest first, without the
    release merges themselves (development -> master)"""
    fields = git("log", "--format=%H%x00%P%x00%s%x00%b%x00", "%s..%s" % (since, until)).split("\x00")
    prs, inside = {}, set()
    for i in range(0, len(fields) - 1, 4):
        sha, parents, subject, body = fields[i].strip(), fields[i + 1], fields[i + 2], fields[i + 3]
        m = re.match(r"Merge pull request #(\d+) from (\S+)", subject)
        if m:
            number, branch = int(m.group(1)), m.group(2)
            if branch.endswith("/development") or branch.endswith("/master"):
                continue  # the release merge
            # the commits of the merged branch: a "(#N)" among them names an issue, not a merge
            first, second = parents.split()[:2]
            inside.update(git("rev-list", "%s..%s" % (first, second)).split())
            title = body.strip().splitlines()[0] if body.strip() else branch
        else:
            m = re.search(r"\(#(\d+)\)$", subject)
            if not m or len(parents.split()) > 1 or sha in inside:
                continue
            number, title = int(m.group(1)), subject[: m.start()].strip()
        prs.setdefault(number, (number, title, sha))
    return sorted(prs.values(), reverse=True)


def changed(sha, paths=()):
    """The files the pull request changed (the merge against its first parent), those the
    pathspecs select when given: git handles the excludes of fw_version.py and deleted files"""
    return git("diff-tree", "--no-commit-id", "--name-only", "-r", "--diff-merges=first-parent", sha, "--", *paths).split()


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


def notes(since, until, versions, upgrade="", intro=""):
    parts = fw_version.parts()
    prs = pull_requests(since, until)
    sections = sort(prs, parts)
    lines = [intro] if intro else []
    for key, name in SECTIONS:
        head = name
        if key in versions:
            head += " %s" % versions[key]
            # each tag model keeps its own version: name them when they differ
            per_model = {k[4:]: v for k, v in versions.items() if k.startswith("tag-")}
            if key == "tag" and len(set(per_model.values())) > 1:
                names = {m["build"]: m["name"] for m in fw_version.models()}
                head += " (%s)" % ", ".join('%s": %s' % (names.get(b, b), v) for b, v in per_model.items())
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
        lines.append("")
    if upgrade:
        lines.append(upgrade)
    return "\n".join(lines).rstrip() + "\n"


def main():
    args = sys.argv[1:]
    if args[:1] in (["-h"], ["--help"]):
        sys.exit(__doc__)
    if args[:1] == ["--check"]:
        version = open(os.path.join(ROOT, "VERSION")).read().strip()
        text = open(os.path.join(ROOT, RELEASE_MD), encoding="utf-8").read()
        summary(text, version, "")  # exits with the error when it is missing
        heading, _ = upgrade_section(text, version) or upgrade_notes("", version, "")
        print("%s: \"Summary of %s\", \"%s\"" % (RELEASE_MD, version, heading))
        return
    tags = fw_version.release_tags() or []
    since = args[0] if args else (tags[0] if tags else None)
    until = args[1] if len(args) > 1 else "HEAD"
    if not since:
        sys.exit("release_notes.py: no release tag to start from")
    # the versions of the commit UNTIL, whatever branch is checked out
    versions = {part: fw_version.version(part, at=until)
                for part in ["ap", "tag"] + ["tag-" + m["build"] for m in fw_version.models()]}
    # the upgrade instructions as they were at UNTIL, for the version it carries
    version = git("show", "%s:VERSION" % until).strip()
    text = git("show", "%s:%s" % (until, RELEASE_MD))
    intro = summary(text, version, "v" + version)
    upgrade = upgrade_notes(text, version, "v" + version)
    sys.stdout.write(notes(since, until, versions, upgrade, intro))


if __name__ == "__main__":
    main()
