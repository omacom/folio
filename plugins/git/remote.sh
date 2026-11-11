# Sourced by the plugin's commands, which run in the plugin's folder, so
# each is given a folder of the repository.

# The page on GitHub of the repository holding folder $1, https://github.com/<owner>/<repo>, from
# the address of its origin, written any of the ways Git takes one
# (git@github.com:o/r.git, ssh://git@github.com/o/r, https://github.com/o/r);
# nothing when it isn't on GitHub.
github() {
  git -C "$1" config --get remote.origin.url 2>/dev/null | sed -nE \
    -e 's#^(ssh://)?git@github\.com[:/]#https://github.com/#' \
    -e 's#^(https?|git)://([^@/]*@)?github\.com/#https://github.com/#' \
    -e 's#(\.git)?/*$##' \
    -e '\#^https://github\.com/[^/]+/[^/]+$#p'
}

# Where the origin is, as a person reads it: owner/repo on GitHub, the host
# and path anywhere else.
remote() {
  page=$(github "$1")
  if [ -n "$page" ]; then
    printf '%s\n' "${page#https://github.com/}"
  else
    git -C "$1" config --get remote.origin.url 2>/dev/null |
      sed -E 's#^[a-z+]+://##; s#^[^@/]*@##; s#^([^/:]+):[0-9]+/#\1/#; s#^([^/:]+):#\1/#; s#(\.git)?/*$##'
  fi
}
