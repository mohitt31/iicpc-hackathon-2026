#!/bin/bash
git filter-branch -f --env-filter '
if [ "$GIT_AUTHOR_EMAIL" = "noreply@anthropic.com" ] || [[ "$GIT_AUTHOR_NAME" == *"Claude"* ]]; then
    export GIT_AUTHOR_NAME="mohitt31"
    export GIT_AUTHOR_EMAIL="mohitprajapati3112@gmail.com"
fi
if [ "$GIT_COMMITTER_EMAIL" = "noreply@anthropic.com" ] || [[ "$GIT_COMMITTER_NAME" == *"Claude"* ]]; then
    export GIT_COMMITTER_NAME="mohitt31"
    export GIT_COMMITTER_EMAIL="mohitprajapati3112@gmail.com"
fi
' --msg-filter '
sed -e "/Co-authored-by: Claude/Id" -e "/Co-Authored-By: Claude/Id"
' -- --all
