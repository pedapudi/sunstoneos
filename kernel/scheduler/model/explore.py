"""Bounded explicit-state checker. Every transition is explored with BFS."""
from collections import deque
from dataclasses import dataclass, replace


def explore(initial, successors, invariant):
    queue = deque([initial])
    parent = {initial: None}
    edges = 0
    while queue:
        state = queue.popleft()
        if not invariant(state):
            trace = []
            while parent[state] is not None:
                prior, action = parent[state]
                trace.append(action)
                state = prior
            return len(parent), edges, list(reversed(trace))
        for action, nxt in successors(state):
            edges += 1
            if nxt not in parent:
                parent[nxt] = (state, action)
                queue.append(nxt)
    return len(parent), edges, None


@dataclass(frozen=True)
class IncomingHead:
    pc: tuple = (0, 0)
    empty: tuple = (False, False)
    inbox: int = 0
    head: bool = False
    detached: int = 0
    observed: int = 0
    flag: bool = False
    pending: bool = False
    owner: int = 0


def incoming_head(priorities=(2, 2), signal_empty=True):
    """SC interleavings of bucket/head publication and the nonurgent doorbell.

    Existing Wake.tla separately checks buffered writes and required DMBs.
    The owner runs priority six throughout: nonurgent work still needs the
    first-publication interrupt to reach its owner queue, without a tick.
    """
    def successors(s):
        def out(name, **kw):
            return name, replace(s, **kw)
        for p in range(2):
            def advance(n):
                return tuple(n if i == p else s.pc[i] for i in range(2))
            if s.pc[p] == 0:
                yield out(f'p{p} release slot bit', pc=advance(1), inbox=s.inbox | (1 << p))
            elif s.pc[p] == 1:
                empty = tuple(not s.head if i == p else s.empty[i] for i in range(2))
                yield out(f'p{p} release head OR returns empty={not s.head}',
                          pc=advance(2), head=True, empty=empty)
            elif s.pc[p] == 2:
                needs = (signal_empty and s.empty[p]) or priorities[p] > 6
                yield out(f'p{p} DMB and dispatch decision needs-SGI={needs}',
                          pc=advance(3 if needs else 5))
            elif s.pc[p] == 3:
                yield out(f'p{p} claim coalescing flag old={s.flag}', flag=True,
                          pc=advance(5 if s.flag else 4))
            elif s.pc[p] == 4:
                yield out(f'p{p} DSB and send SGI', pending=True, pc=advance(5))
        if s.owner == 0 and s.pending:
            yield out('owner enter SGI', owner=1, pending=False)
        elif s.owner == 1:
            yield out('owner acquire detach head', owner=2, head=False)
        elif s.owner == 2:
            yield out('owner acquire detach bucket', owner=3,
                      detached=s.inbox, inbox=0)
        elif s.owner == 3:
            yield out('owner inspect detached slots', owner=4, detached=0,
                      observed=s.observed | s.detached)
        elif s.owner == 4:
            yield out('owner release pending clear', owner=5, flag=False)
        elif s.owner == 5:
            remaining = s.head or bool(s.inbox)
            yield out(f'owner DMB and final all-work check remaining={remaining}',
                      owner=6 if remaining else 0)
        elif s.owner == 6:
            yield out('owner request bounded continuation', owner=0,
                      pending=s.pending or not s.flag, flag=True)
    def invariant(s):
        finished = all(pc == 5 for pc in s.pc)
        quiescent = s.owner == 0 and not s.pending
        return not (finished and quiescent and
                    (s.inbox or s.head or s.detached or s.observed != 3))
    return IncomingHead(), successors, invariant


@dataclass(frozen=True)
class LastPut:
    put: int = 0
    close: int = 0
    reference_buffer: bool = False
    close_buffer: bool = False
    reference: bool = True
    closed: bool = False
    notified: bool = False


def last_put_close(put_barrier=True):
    """Both the closer and last release must not miss the retirement event."""
    def successors(s):
        def out(name, **kw):
            return name, replace(s, **kw)
        if s.put == 0:
            yield out('last put release decrement buffered', put=1, reference_buffer=True)
        if s.reference_buffer:
            yield out('propagate zero references', reference_buffer=False, reference=False)
        if s.put == 1 and (not put_barrier or not s.reference_buffer):
            ordering = 'DMB then ' if put_barrier else 'no DMB before '
            yield out('last put '+ordering+'acquire admission', put=2,
                      notified=s.notified or s.closed)
        if s.close == 0:
            yield out('close release admission buffered', close=1, close_buffer=True)
        if s.close_buffer:
            yield out('propagate closed admission', close_buffer=False, closed=True)
        if s.close == 1 and not s.close_buffer:
            yield out('closer DMB then acquire references', close=2,
                      notified=s.notified or not s.reference)
    return LastPut(), successors, lambda s: not (
        s.put == 2 and s.close == 2 and not s.notified)


@dataclass(frozen=True)
class MigrationAck:
    # One coordinator reference and one already-pinned old-route publisher.
    references: int = 2
    publisher: int = 0
    live: bool = False
    detached: bool = False
    old_ack: bool = False
    retired: bool = False


def migration_ack(ack_after_publishers=True):
    """A remembered empty inbox cannot acknowledge a still-pinned publisher."""
    def successors(s):
        def out(name, **kw):
            return name, replace(s, **kw)
        if s.publisher == 0:
            yield out('old-route publisher release-publishes late hint',
                      publisher=1, live=True)
        elif s.publisher == 1:
            yield out('old-route publisher releases epoch reference',
                      publisher=2, references=1)
        if s.live and not s.detached:
            yield out('old owner detaches hint', live=False, detached=True)
        if s.detached:
            yield out('old owner forwards detached hint', detached=False)
        if not s.old_ack and not s.live and not s.detached and (
                not ack_after_publishers or s.references == 1):
            yield out('old owner acknowledges drained epoch', old_ack=True)
        if s.old_ack and s.references == 1 and not s.retired:
            yield out('destination ends old forwarding epoch', retired=True)
    return MigrationAck(), successors, lambda s: not (
        s.retired and (s.live or s.detached))


@dataclass(frozen=True)
class Notify:
    word: tuple = (0, 'WAITING', False, 0)
    pc: tuple = (0, 0)
    snapshots: tuple = (None, None)
    turns: tuple = (0, 0)
    queue: tuple = ()
    ticketed: tuple = ()
    holder: int = -1
    payload: tuple = ()
    owner: int = 0
    generation: int = 0
    won: bool = False
    ready: int = 0
    hint: bool = False
    detached: bool = False
    bad: bool = False


def notification(actions, sticky=True, register_first=True, observe_gate=True):
    def transform(word, action):
        value, state, latch, count = word
        if action == 'clear':
            return (0, 'NOT_WAITING', latch if sticky else False, count)
        if action == 'bits':
            value |= 2
        elif action == 'give':
            value += 1
        elif action == 'overwrite':
            value = 3
        elif action == 'nooverwrite' and state == 'RECEIVED':
            return word
        if state == 'WAITING':
            latch = True
        return (value, 'RECEIVED', latch, count)

    def observe(word):
        # The same read determines readiness for gated and ungated consumers.
        return bool(word[2] or word[1] == 'RECEIVED')

    def successors(s):
        def out(name, **kw):
            return name, replace(s, **kw)
        for p in range(2):
            pc = s.pc[p]
            def advance(n):
                return tuple(n if i == p else s.pc[i] for i in range(2))
            if pc == 0:
                snap = tuple(s.word if i == p else s.snapshots[i] for i in range(2))
                yield out(f'p{p} acquire snapshot', pc=advance(1), snapshots=snap)
            elif pc == 1:
                old = s.snapshots[p]
                if old == s.word and old[3] == 0:
                    word = transform(s.word, actions[p])
                    yield out(f'p{p} matched single CAS {actions[p]}', pc=advance(7),
                              word=word, won=s.won or word[2], bad=s.bad or s.holder != -1)
                else:
                    yield out(f'p{p} CAS fails once', pc=advance(2))
            elif pc == 2:
                word = s.word[:3] + (s.word[3] + 1,)
                if register_first:
                    yield out(f'p{p} register count before ticket', word=word, pc=advance(3))
                else:
                    yield out(f'p{p} ticket before registration', pc=advance(4), queue=s.queue+(p,), ticketed=s.ticketed+(p,))
            elif pc == 3:
                yield out(f'p{p} obtains FIFO ticket', pc=advance(4), queue=s.queue+(p,))
            elif pc == 4 and s.holder == -1 and s.queue and s.queue[0] == p:
                # Broken variant exposes a transient payload while count is zero.
                word = s.word
                yield out(f'p{p} gated clear', word=(0,'NOT_WAITING',False,word[3]),
                          holder=p, payload=transform(word, actions[p])[:3], pc=advance(5))
            elif pc == 5:
                yield out(f'p{p} gated set preserving registration',
                          word=s.payload+(s.word[3],), pc=advance(6),
                          won=s.won or s.payload[2])
            elif pc == 6:
                yield out(f'p{p} release registration and serving',
                          word=s.word[:3]+(s.word[3]-int(register_first),), holder=-1,
                          queue=s.queue[1:], pc=advance(7))
            elif pc == 8 and s.turns[p] == 0:
                turns=tuple(1 if i==p else x for i,x in enumerate(s.turns))
                yield out(f'p{p} second API action', pc=advance(0), turns=turns)
            elif pc == 7:
                yield out(f'p{p} publish level hint', hint=True, pc=advance(8))
        if s.hint and not s.detached:
            yield out('owner acquire detach hint', hint=False, detached=True)
        if s.owner == 0 and (s.detached or all(p == 8 for p in s.pc)):
            yield out('owner registers before ticket', owner=1,
                      word=s.word[:3]+(s.word[3]+1,))
        if s.owner == 1:
            yield out('owner gets FIFO ticket', owner=2, queue=s.queue+(2,))
        if s.owner == 2 and s.holder == -1 and s.queue and s.queue[0] == 2:
            completed = observe(s.word)
            yield out('owner stable consume or timeout winner', owner=3, holder=2,
                      ready=int(completed), bad=s.bad or (s.won and not completed))
        if s.owner == 3:
            yield out('owner releases gate', owner=4, holder=-1, queue=s.queue[1:],
                      word=s.word[:3]+(s.word[3]-1,), detached=False)
        if s.owner == 4 and s.generation == 0:
            yield out('next wait registers gate', owner=5,
                      word=s.word[:3]+(s.word[3]+1,))
        if s.owner == 5:
            yield out('next wait takes FIFO ticket', owner=6, queue=s.queue+(2,))
        if s.owner == 6 and s.holder == -1 and s.queue and s.queue[0] == 2:
            yield out('next wait gated payload clear', owner=7, holder=2,
                      word=(0,'NOT_WAITING',False,s.word[3]))
        if s.owner == 7:
            yield out('next wait commits PREPARING and WAITING', owner=8,
                      word=(0,'WAITING',False,s.word[3]), generation=1, won=False, ready=0)
        if s.owner == 8:
            yield out('next wait opens gate before physical switch', owner=0, holder=-1,
                      queue=s.queue[1:], word=s.word[:3]+(s.word[3]-1,))
        if not observe_gate and s.owner == 0 and s.detached:
            completed = observe(s.word)
            yield out(f'owner ungated payload read {s.word[:3]}', owner=4,
                      ready=int(completed), detached=False,
                      bad=s.bad or (s.won and not completed))
    return Notify(), successors, lambda s: not s.bad


@dataclass(frozen=True)
class Retire:
    pc: tuple = (0, 0)
    refbuf: frozenset = frozenset()
    refs: frozenset = frozenset()
    closebuf: bool = False
    closed: bool = False
    deletion: int = 0
    admitted: frozenset = frozenset()
    live: frozenset = frozenset()
    detached: frozenset = frozenset()
    reused: bool = False
    bad: bool = False


def retirement(pin_barrier=True, close_barrier=True, drain=True):
    def successors(s):
        def out(name, **kw): return name, replace(s, **kw)
        for p, pc in enumerate(s.pc):
            def advance(n): return tuple(n if i == p else x for i,x in enumerate(s.pc))
            if pc == 0:
                yield out(f'p{p} buffer reference increment', pc=advance(1), refbuf=s.refbuf|{p})
            if p in s.refbuf:
                yield out(f'p{p} propagate coherent ref increment', refbuf=s.refbuf-{p}, refs=s.refs|{p})
            if pc == 1 and (not pin_barrier or p not in s.refbuf):
                yield out(f'p{p} acquire admission', pc=advance(2 if not s.closed else 4),
                          admitted=s.admitted|({p} if not s.closed else set()),
                          bad=s.bad or (s.reused and not s.closed))
            if pc == 2:
                yield out(f'p{p} delayed publication', pc=advance(3), live=s.live|{p}, bad=s.bad or s.reused)
            if pc == 3:
                yield out(f'owner detaches p{p}', pc=advance(4), live=s.live-{p}, detached=s.detached|{p})
            if pc == 4 and p not in s.refbuf:
                yield out(f'p{p} reference retire', pc=advance(5), refs=s.refs-{p}, admitted=s.admitted-{p})
        if s.deletion == 0:
            yield out('deletion release closes admission', deletion=1, closebuf=True)
        if s.closebuf:
            yield out('propagate coherent closed admission', closed=True, closebuf=False)
        if s.deletion == 1 and (not close_barrier or not s.closebuf) and not s.refs:
            yield out('deletion observes reference zero', deletion=2)
        if s.detached and s.deletion == 2:
            yield out('owners drain detached and live slots', detached=frozenset(), live=frozenset())
        if s.deletion == 2 and (not drain or not s.detached and not s.live):
            yield out('reuse incarnation after owner acknowledgments', deletion=3, reused=True,
                      bad=s.bad or bool(s.admitted) or bool(s.detached) or bool(s.live))
    # Late callers have old handles; closed admission remains closed in this
    # bounded retirement phase. Reopening with a valid new handle is separate.
    return Retire(), successors, lambda s: not s.bad


@dataclass(frozen=True)
class Migration:
    pc: tuple = (0, 0)
    routes: tuple = (-1, -1)
    owner: int = 0
    phase: int = 0
    live: tuple = (False, False)
    detached: tuple = (False, False)
    complete: bool = False
    ready: bool = False
    retired: bool = False
    priority: int = 0
    raised: bool = False
    urgent: bool = False
    bad: bool = False


def migration(forward=True, retain=True, republish=True):
    def successors(s):
        def out(name, **kw): return name, replace(s, **kw)
        for p, pc in enumerate(s.pc):
            def advance(n): return tuple(n if i == p else x for i,x in enumerate(s.pc))
            if pc == 0 and not s.retired:
                routes=tuple(s.owner if i == p else x for i,x in enumerate(s.routes))
                yield out(f'p{p} pin and snapshot owner epoch', pc=advance(1),routes=routes)
            elif pc == 1:
                live=tuple(True if i == s.routes[p] else x for i,x in enumerate(s.live))
                yield out(f'p{p} complete and delayed publish old/new owner', pc=advance(2),live=live,complete=True,urgent=s.urgent or s.priority==1)
            elif pc == 2:
                yield out(f'p{p} retire epoch reference', pc=advance(3))
        if s.phase == 0:
            yield out('old owner off-CPU transfer release',phase=1)
        if s.phase == 1:
            yield out('destination acquire adopts and checks completion',phase=2,owner=1,ready=s.complete)
        for o in range(2):
            if s.live[o] and not s.detached[o]:
                live=tuple(False if i==o else x for i,x in enumerate(s.live))
                detached=tuple(True if i==o else x for i,x in enumerate(s.detached))
                yield out(f'owner{o} detaches fixed-slot bit',live=live,detached=detached)
            if s.detached[o]:
                detached=tuple(False if i==o else x for i,x in enumerate(s.detached))
                if o==s.owner:
                    yield out(f'owner{o} gates authoritative completion',detached=detached,ready=s.ready or s.complete)
                elif s.phase==2:
                    live=(s.live[0],s.live[1] or (forward and not s.retired))
                    yield out('old owner forwards late old-epoch hint',detached=detached,live=live)
        if s.phase==2 and not s.retired and (not retain or all(p==3 for p in s.pc) and not any(s.live) and not any(s.detached)):
            yield out('both owners acknowledge forwarding retirement',retired=True)
        if not s.raised:
            yield out('effective priority raise republishes detached work',raised=True,priority=1,
                      urgent=republish and (s.complete or any(s.live) or any(s.detached)))
        if s.raised and s.complete and (any(s.live) or any(s.detached)) and not s.ready and not s.urgent:
            yield out('owner commits lower-priority work without urgent repair',bad=True)
        terminal=all(p==3 for p in s.pc) and s.phase==2 and not any(s.live) and not any(s.detached)
        if terminal and s.complete and not s.ready:
            yield out('quiescent completed wait lost',bad=True)
    return Migration(), successors, lambda s: not s.bad


@dataclass(frozen=True)
class Block:
    gate: bool = False
    owner: int = 0
    execution: str = 'RUNNING'
    winner: str = ''
    pc: tuple = (0, 0)
    hint: bool = False
    latch: bool = False
    notified: bool = False
    timeout: bool = False
    activations: int = 0
    bad: bool = False


def block_race(winner_check=True):
    def successors(s):
        def out(name, **kw): return name, replace(s, **kw)
        if s.owner == 0:
            yield out('wait entry acquires slot gate', gate=True, owner=1)
        if s.owner == 1:
            yield out('wait entry commits PREPARING',execution='PREPARING',owner=2)
        if s.owner == 2:
            yield out('wait entry opens gate',gate=False,owner=3)
        if s.owner == 3 and not s.gate:
            ready=bool(s.winner)
            yield out('final latch check and physical switch',owner=4,
                      execution='READY' if ready else 'BLOCKED',activations=int(ready))
        for p, pc in enumerate(s.pc):
            def advance(n): return tuple(n if i==p else x for i,x in enumerate(s.pc))
            if pc == 0 and not s.gate and s.owner >= 3:
                wins=not s.winner
                yield out(f'p{p} atomic WAITING to RECEIVED plus latch',pc=advance(1),
                          winner='NOTIFY' if wins else s.winner,
                          latch=s.latch or wins,notified=s.notified or wins)
            if pc == 1:
                yield out(f'p{p} delayed level hint',pc=advance(2),hint=True)
        if not s.timeout and not s.gate and s.owner>=3:
            winner=s.winner if winner_check and s.latch else (s.winner or 'TIMEOUT')
            if not winner_check:
                winner='TIMEOUT'
            yield out('timeout gated winner check',timeout=True,winner=winner,
                      bad=s.bad or (s.notified and winner!='NOTIFY'))
        if s.hint and not s.gate:
            activate=s.execution=='BLOCKED' and bool(s.winner)
            yield out('owner gates hint and activates current wait once',hint=False,
                      execution='READY' if activate else s.execution,
                      activations=s.activations+int(activate))
        if s.timeout and s.execution=='BLOCKED':
            yield out('owner applies timeout completion once',execution='READY',
                      activations=s.activations+1)
    return Block(),successors,lambda s: not s.bad and s.activations<=1
