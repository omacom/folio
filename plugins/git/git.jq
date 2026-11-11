# Read by the plugin's commands: what `git status --porcelain=v2 -z
# --branch` prints, and how a path stands in it.

# Each header ({header, value}: "branch.head", "main") and each changed
# path ({xy, path}, relative to the top of the repository) in turn. A
# rename's entry is followed by its old path, which is skipped.
def records:
  foreach (split("\u0000")[] | select(. != "")) as $f ({skip: false};
    if .skip then {skip: false, out: null}
    elif $f[0:2] == "# " then ($f[2:] | index(" ")) as $i
      | {skip: false, out: {header: $f[2:2 + $i], value: $f[3 + $i:]}}
    else ($f[0:1]) as $type
      | {skip: ($type == "2"),
         out: {xy: (if $type == "?" then "??" elif $type == "!" then "!!" else $f[2:4] end),
               path: ($f | split(" ") | .[{"1": 8, "2": 9, "u": 10, "?": 1, "!": 1}[$type]:] | join(" "))}}
    end;
    .out | select(. != null));

# How a path with these two status letters stands: both sides changing it
# is a conflict, and a path gone is "deleted", which shows nowhere.
def kind:
  if test("U") or . == "AA" or . == "DD" then "conflict"
  elif . == "??" then "untracked"
  elif . == "!!" then "ignored"
  elif test("D") then "deleted"
  elif .[0:1] == "A" or .[0:1] == "R" or .[0:1] == "C" then "added"
  else "modified" end;

# The mark for a kind, as Folio shows it.
def mark:
  {modified: {tone: "modified", badge: "M", label: "Modified"},
   added: {tone: "added", badge: "A", label: "New"},
   untracked: {tone: "untracked", badge: "?", label: "Not tracked"},
   conflict: {tone: "conflict", badge: "C", label: "Conflict"}}[.];

# The branch from the headers: its name (a detached HEAD's short hash) and
# how far it's ahead of and behind its upstream, leaving out what's 0.
def branch:
  ((.["branch.ab"] // "+0 -0") | capture("\\+(?<ahead>\\d+) -(?<behind>\\d+)")) as $ab
  | {name: (if .["branch.head"] == "(detached)" then .["branch.oid"][:7] else .["branch.head"] end),
     ab: ([if $ab.ahead != "0" then "↑" + $ab.ahead else empty end,
           if $ab.behind != "0" then "↓" + $ab.behind else empty end] | join(" "))};
