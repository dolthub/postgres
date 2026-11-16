#!/bin/bash
#
# Copyright 2026 Dolthub, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# post.sh <comments folder> <repository> <head sha>: posts the comments a pull request's test run left as its
# artifact, each replacing the earlier comment of the bot that carries the same marker, so each suite keeps one comment
# that every push updates, as Doltgres' regression workflow does. The folder holds the pull request's number in
# `pr-number` and one `<suite>.md` per comment, whose first line is its marker. The artifact came from a run of the
# pull request's code, so it is read as data: the number must name a pull request whose head is the tested commit, and
# a file whose first line isn't a marker is refused. GH_TOKEN must allow writing pull request comments.
set -euo pipefail

# GitHub refuses comment bodies over 65536 characters.
max=65000
number=$(cat "$1/pr-number")
if ! [[ "$number" =~ ^[0-9]+$ ]]; then
  echo "post.sh: pr-number isn't a number" >&2
  exit 1
fi
if [ "$(gh api "repos/$2/pulls/$number" --jq .head.sha)" != "$3" ]; then
  echo "post.sh: pull request $number's head isn't the tested commit $3; it moved on, so the newer run posts" >&2
  exit 0
fi
for file in "$1"/*.md; do
  marker=$(head -n 1 "$file")
  if ! [[ "$marker" =~ ^\<\!--\ pgfork-ci:[a-z0-9-]+\ --\>$ ]]; then
    echo "post.sh: $file doesn't start with a pgfork-ci marker" >&2
    exit 1
  fi
  body=$(cat "$file")
  if [ ${#body} -gt $max ]; then
    body="${body:0:$((max - 100))}"$'\n\n'"_Truncated; the workflow run has the whole comment as an artifact._"
  fi
  existing=$(gh api --paginate "repos/$2/issues/$number/comments" \
    --jq ".[] | select(.user.login == \"github-actions[bot]\" and (.body | startswith(\"$marker\"))) | .id" | sed -n 1p)
  if [ -n "$existing" ]; then
    gh api -X PATCH "repos/$2/issues/comments/$existing" -f body="$body" > /dev/null
  else
    gh api -X POST "repos/$2/issues/$number/comments" -f body="$body" > /dev/null
  fi
done
