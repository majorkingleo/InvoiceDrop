#!/usr/bin/env bash
#
# What this Ollama server can do right now, and what one call costs.
#
# Usage: ollama-probe.sh [MODEL]
#
# Reports reachability, the models that are pulled, what is resident in memory,
# the capabilities of the wanted model, and one timed call. The two numbers worth
# comparing are total_duration and load_duration: when load_duration is most of
# the total, the model was not resident and every call pays the load again.
#
# Exit 0 when the server answers and the model is pulled, 2 otherwise.

set -uo pipefail

url="${OLLAMA_HOST:-http://127.0.0.1:11434}"
case "$url" in
    http://* | https://*) ;;
    *) url="http://$url" ;; # OLLAMA_HOST is usually host:port, with no scheme
esac
url="${url%/}" # a trailing slash would make every path start with //

model="${1:-gemma4:latest}"
fail=0

for tool in curl jq; do
    if ! command -v "$tool" >/dev/null; then
        printf '%s is needed for this script\n' "$tool" >&2
        exit 2
    fi
done

printf 'endpoint  %s\n' "$url"

tags="$(curl -sS --max-time 5 "$url/api/tags" 2>/dev/null)"
if [ -z "$tags" ]; then
    printf '  FAIL  no answer. Start the server, then try again:\n'
    printf '          systemctl --user start ollama   # or: systemctl start ollama\n'
    exit 2
fi

printf '  ok    %s model(s) pulled\n' "$(printf '%s' "$tags" | jq '.models | length')"
printf '%s' "$tags" | jq -r '.models[].name' | sed 's/^/          /'

# An exact name, or nothing. Only a request with no tag at all may resolve to
# another tag, and then to `<base>:latest` first, which is what `ollama run
# <base>` does. Comparing base names unconditionally is wrong in a way that looks
# right: `gemma4:26b` is not `gemma4:latest`, and it is a different, slower model.
case "$model" in
    *:*)
        pulled="$(printf '%s' "$tags" | jq -r --arg m "$model" \
            '.models[].name | select(. == $m) | .' | head -n 1)"
        ;;
    *)
        pulled="$(printf '%s' "$tags" | jq -r --arg m "$model" \
            '.models[].name | select(. == ($m + ":latest")) | .' | head -n 1)"
        if [ -z "$pulled" ]; then
            pulled="$(printf '%s' "$tags" | jq -r --arg m "$model" \
                '.models[].name | select(split(":")[0] == $m) | .' | head -n 1)"
        fi
        ;;
esac

if [ -z "$pulled" ]; then
    printf '  FAIL  %s is not pulled\n' "$model"
    printf '          ollama pull %s\n' "$model"
    fail=1
else
    printf '  ok    %s is pulled\n' "$pulled"
fi

# What is already loaded, so a cold start can be told from a slow model.
loaded="$(curl -sS --max-time 5 "$url/api/ps" 2>/dev/null | jq -r '.models[].name' 2>/dev/null)"
if [ -n "$loaded" ]; then
    printf '  ok    resident now: %s\n' "$(printf '%s' "$loaded" | tr '\n' ' ')"
else
    printf '  --    nothing is resident: the next call pays the load\n'
fi

if [ -n "$pulled" ]; then
    # `capabilities` is where "can this model see" is answered. A model without
    # vision does not fail on an image, it ignores it.
    caps="$(curl -sS --max-time 10 "$url/api/show" -d "{\"model\":\"$pulled\"}" 2>/dev/null |
        jq -r '(.capabilities // []) | join(" ")' 2>/dev/null)"
    size="$(curl -sS --max-time 10 "$url/api/show" -d "{\"model\":\"$pulled\"}" 2>/dev/null |
        jq -r '.details.parameter_size // "unknown"' 2>/dev/null)"

    if [ -n "$caps" ]; then
        printf '  ok    %s: %s (%s)\n' "$pulled" "$caps" "$size"
        case " $caps " in
            *" vision "*) ;;
            *) printf '  --    no vision capability: this model ignores images\n' ;;
        esac
    fi
fi

# One call, with the settings this skill recommends. The prompt is deliberately
# trivial: the point is the timing, not the answer. Skipped when the model is known
# not to be pulled, because the server would only answer "not found" a second time.
if [ "$fail" -eq 0 ]; then
    body="$(printf '%s' \
        "{\"model\":\"$model\",\"stream\":false,\"think\":false,\"keep_alive\":\"30m\",\"options\":{\"temperature\":0},\"messages\":[{\"role\":\"user\",\"content\":\"Reply with the single word: ok\"}]}")"

    started="$(date +%s%N)"
    reply="$(curl -sS --max-time 300 "$url/api/chat" -d "$body" 2>/dev/null)"
    elapsed="$(( ( $(date +%s%N) - started ) / 1000000 ))"

    if printf '%s' "$reply" | jq -e '.error' >/dev/null 2>&1; then
        printf '  FAIL  %s\n' "$(printf '%s' "$reply" | jq -r '.error')"
        fail=1
    elif [ -z "$reply" ]; then
        printf '  FAIL  no answer within 300 s: the model may still be loading\n'
        fail=1
    else
        # Durations come back in nanoseconds. load_duration being most of the total
        # is what a cold start looks like, and it is the difference between "slow
        # model" and "keep_alive was not honoured".
        read -r total_ms load_ms tokens < <(printf '%s' "$reply" | jq -r \
            '"\((.total_duration // 0) / 1000000 | floor) \((.load_duration // 0) / 1000000 | floor) \(.eval_count // 0)"')

        printf '  ok    answered in %s ms wall clock\n' "$elapsed"
        printf '  ok    total %s ms, load %s ms, %s token(s)\n' "$total_ms" "$load_ms" "$tokens"
        printf '  ok    done_reason: %s\n' "$(printf '%s' "$reply" | jq -r '.done_reason // "-"')"

        # Where the time went, which is all these two numbers can say: loading
        # weights and generating tokens are the only two things in the total.
        if [ "$load_ms" -gt 0 ] && [ "$load_ms" -gt $(( total_ms / 2 )) ]; then
            printf '  --    most of the total was load_duration: the time went into weights, not generation\n'
        fi
    fi
fi

if [ "$fail" -ne 0 ]; then
    exit 2
fi
