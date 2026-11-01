------------------------------ MODULE Wake ------------------------------
EXTENDS Naturals, FiniteSets
CONSTANTS ProducerBarrier, OwnerBarrier, ClearBeforeCheck
P == {1, 2}
VARIABLES pc, buffered, inbox, dispatchBuffer, idle, owner, flag,
          clearBuffer, pending, active, observed
vars == <<pc, buffered, inbox, dispatchBuffer, idle, owner, flag,
          clearBuffer, pending, active, observed>>
Init == /\ pc = [p \in P |-> 0] /\ buffered = {} /\ inbox = {}
        /\ dispatchBuffer = FALSE /\ idle = FALSE /\ owner = 0
        /\ flag = TRUE /\ clearBuffer = FALSE /\ pending = FALSE
        /\ active = TRUE /\ observed = FALSE
Publish(p) == /\ pc[p] = 0 /\ pc' = [pc EXCEPT ![p] = 1]
              /\ buffered' = buffered \cup {p}
              /\ UNCHANGED <<inbox, dispatchBuffer, idle, owner, flag,
                             clearBuffer, pending, active, observed>>
PropagateHint(p) == /\ p \in buffered /\ buffered' = buffered \ {p}
                    /\ inbox' = inbox \cup {p}
                    /\ UNCHANGED <<pc, dispatchBuffer, idle, owner, flag,
                                   clearBuffer, pending, active, observed>>
ReadDispatch(p) == /\ pc[p] = 1 /\ (~ProducerBarrier \/ p \notin buffered)
                   /\ pc' = [pc EXCEPT ![p] = IF idle THEN 2 ELSE 4]
                   /\ UNCHANGED <<buffered, inbox, dispatchBuffer, idle,
                         owner, flag, clearBuffer, pending, active, observed>>
Claim(p) == /\ pc[p] = 2 /\ pc' = [pc EXCEPT ![p] = IF flag THEN 4 ELSE 3]
            /\ flag' = TRUE
            /\ UNCHANGED <<buffered, inbox, dispatchBuffer, idle, owner,
                           clearBuffer, pending, active, observed>>
Send(p) == /\ pc[p] = 3 /\ pc' = [pc EXCEPT ![p] = 4] /\ pending' = TRUE
           /\ UNCHANGED <<buffered, inbox, dispatchBuffer, idle, owner,
                          flag, clearBuffer, active, observed>>
Lower == /\ owner = 0 /\ owner' = 1 /\ dispatchBuffer' = TRUE
         /\ clearBuffer' = ClearBeforeCheck
         /\ UNCHANGED <<pc, buffered, inbox, idle, flag, pending, active, observed>>
PropagateDispatch == /\ dispatchBuffer /\ dispatchBuffer' = FALSE
                     /\ idle' = TRUE
                     /\ UNCHANGED <<pc, buffered, inbox, owner, flag,
                                    clearBuffer, pending, active, observed>>
PropagateClear == /\ clearBuffer /\ clearBuffer' = FALSE /\ flag' = FALSE
                  /\ UNCHANGED <<pc, buffered, inbox, dispatchBuffer, idle,
                                 owner, pending, active, observed>>
Check == /\ owner = 1
         /\ (~OwnerBarrier \/ (~dispatchBuffer /\ ~clearBuffer))
         /\ observed' = (inbox # {}) /\ owner' = 2
         /\ UNCHANGED <<pc, buffered, inbox, dispatchBuffer, idle, flag,
                        clearBuffer, pending, active>>
Exit == /\ owner = 2 /\ owner' = 3 /\ active' = FALSE
        /\ clearBuffer' = IF ClearBeforeCheck THEN clearBuffer ELSE TRUE
        /\ UNCHANGED <<pc, buffered, inbox, dispatchBuffer, idle, flag,
                       pending, observed>>
Next == (\E p \in P: Publish(p) \/ PropagateHint(p) \/ ReadDispatch(p)
                        \/ Claim(p) \/ Send(p))
        \/ Lower \/ PropagateDispatch \/ PropagateClear \/ Check \/ Exit
NoLostWake == ~(/\ owner = 3 /\ ~observed /\ ~pending /\ ~active
                /\ \A p \in P: pc[p] = 4)
Spec == Init /\ [][Next]_vars
=============================================================================
