"""Stand-ins for the `gh` adapter and the dispatcher's registry.

FakeGh replays recorded GraphQL responses. Fixtures are JSON files in
tests/fixtures/. Each one maps a GraphQL operation name (the word after
`query` in the request, e.g. `FindBoard`) to the response GitHub sent
back. Paged operations map the page cursor they were asked for ("" for
the first page) to that page's response. A fixture's "rest" entry maps a
REST path to the list GitHub returns for it, for the tests that run a
whole tick.

Every request is recorded, so a test can prove the tick only read from
GitHub: `mutations` lists any request that would have changed something.

FakeRegistry is the dispatcher's memory with no sessions in it, for the
tests that run a whole tick.
"""

import json
import os
import re

FIXTURES = os.path.join(os.path.dirname(__file__), "fixtures")

_OPERATION = re.compile(r"^\s*(query|mutation)\s+(\w+)")


class FakeGh:
    def __init__(self, responses):
        self.responses = responses
        self.requests = []

    @classmethod
    def from_fixture(cls, name):
        with open(os.path.join(FIXTURES, name)) as f:
            return cls(json.load(f))

    def graphql(self, query, **variables):
        match = _OPERATION.match(query)
        if not match:
            raise AssertionError("request has no operation name: " + query[:60])
        kind, name = match.groups()
        self.requests.append((kind, name, variables))
        if kind == "mutation":
            return {"data": {}}
        response = self.responses[name]
        if "cursor" in variables:
            response = response[variables["cursor"] or ""]
        return response

    def rest_list(self, path):
        self.requests.append(("rest", path, {}))
        return self.responses["rest"][path]

    @property
    def mutations(self):
        return [r for r in self.requests if r[0] == "mutation"]


class FakeRegistry:
    """The dispatcher's memory with no sessions in it. Records the sessions
    a tick starts, as (role, issue)."""

    def __init__(self):
        self.started = []

    def load(self, now=None):
        return []

    def paused_until(self):
        return 0.0

    def crashes(self):
        return {}

    def requeued(self):
        return set()

    def start(self, role, issue, prompt, pr=None, handled_through=None):
        self.started.append((role, issue))

    def prune(self):
        return 0
