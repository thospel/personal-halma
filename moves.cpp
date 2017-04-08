NOINLINE
uint64_t NAME(BoardSet& boards_from,
              BoardSet& boards_to,
              ArmySet const& moving_armies,
              ArmySet const& opponent_armies,
              ArmySet& moved_armies,
#if BACKTRACK && !BLUE_TO_MOVE
              BoardTable<uint8_t> const& backtrack,
#endif // BACKTRACK && !BLUE_TO_MOVE
              int available_moves) {
#if !BLUE_TO_MOVE
    int balance_moves = max(available_moves/2 + balance_delay - ARMY, 0);
    BalanceMask balance_mask = make_balance_mask(balance_min-balance_moves, balance_max+balance_moves);
    balance_mask = ~balance_mask;
    ParityCount balance_count_from, balance_count;
#endif
    uint64_t late = 0;
    while (true) {
        BoardSubSetRef subset{boards_from};

        ArmyId blue_id = subset.id();
        if (blue_id == 0) break;
        if (VERBOSE) cout << "Processing blue " << blue_id << "\n";
        Army const& b = BLUE_TO_MOVE ?
            moving_armies.at(blue_id) :
            opponent_armies.at(blue_id);
        if (CHECK) b.check(__LINE__);
        Army const b_symmetric = b.symmetric();
#if BLUE_TO_MOVE
        ArmyMapper const b_mapper{b_symmetric};
        ArmyMapper const b_mapper_symmetric{b};
#else  // BLUE_TO_MOVE
        // Will be either 0 or positive
        int b_symmetry = cmp(b, b_symmetric);
#endif  // BLUE_TO_MOVE

        BoardSubSet const& red_armies = subset.armies();
        for (auto const& red_value: red_armies) {
            if (red_value == 0) continue;
            ArmyId red_id;
            auto symmetry = BoardSubSet::split(red_value, red_id);
            if (VERBOSE) cout << " Sub Processing red " << red_id << "," << symmetry << "\n";
            Army const& blue         = symmetry ? b_symmetric : b;
#if BLUE_TO_MOVE
            Army const& blue_symmetric = symmetry ? b : b_symmetric;
            ArmyMapper const& mapper = symmetry ? b_mapper_symmetric : b_mapper;
#else  // BLUE_TO_MOVE
            int  blue_symmetry       = symmetry ? -b_symmetry : b_symmetry;
#endif // BLUE_TO_MOVE

            Army const& red = BLUE_TO_MOVE ?
                opponent_armies.at(red_id) :
                moving_armies.at(red_id);
            if (CHECK) red.check(__LINE__);

#if VERBOSE
            Image image{blue, red};
            cout << "  From: [" << blue_id << ", " << red_id << ", " << symmetry << "] " << available_moves << " moves\n" << image;
#endif // VERBOSE

            Nbits Ndistance_army, Ndistance_red;
            Ndistance_army = Ndistance_red = NLEFT >> tables.infinity();
            int off_base_from = 0;
            ParityCount parity_count_from = tables.parity_count();
            int edge_count_from = 0;
            for (auto const& b: blue) {
                --parity_count_from[b.parity()];
                if (tables.base_red(b)) continue;
                ++off_base_from;
                edge_count_from += tables.edge_red(b);
                Ndistance_red |= tables.Ndistance_base_red(b);
                for (auto const& r: red)
                    Ndistance_army |= tables.Ndistance(r, b);
            }
            int slides = 0;
            for (auto tc: parity_count_from)
                slides += max(tc, 0);
            int distance_army = __builtin_clz(Ndistance_army);
            int distance_red  = __builtin_clz(Ndistance_red);

            if (VERBOSE) {
                cout << "  Slides >= " << slides << ", red edge count " << edge_count_from << "\n";
                cout << "  Distance army=" << distance_army << "\n";
                cout << "  Distance red =" << distance_red  << "\n";
                cout << "  Off base=" << off_base_from << "\n";
            }
            int pre_moves = min((distance_army + BLUE_TO_MOVE) / 2, distance_red);
            int blue_moves = pre_moves + max(slides-pre_moves-edge_count_from, 0) + off_base_from;
            int needed_moves = 2*blue_moves - BLUE_TO_MOVE;
            if (VERBOSE)
                cout << "  Needed moves=" << static_cast<int>(needed_moves) << "\n";
            if (needed_moves > available_moves) {
                if (VERBOSE)
                    cout << "  Late prune " << needed_moves << " > " << available_moves << "\n";
                ++late;
                continue;
            }

            Army red_symmetric = red.symmetric();
#if BLUE_TO_MOVE
            int red_symmetry = cmp(red, red_symmetric);
            Army const& army           = blue;
            Army const& army_symmetric = blue_symmetric;
#else  // BLUE_TO_MOVE
            Army const& army           = red;
            Army const& army_symmetric = red_symmetric;
            ArmyMapper const mapper{red_symmetric};

#if BACKTRACK
            int backtrack_count_from = 2*ARMY;
            int backtrack_count_symmetric_from = 2*ARMY;
            for (auto const& pos: army)
                backtrack_count_from -= backtrack[pos];
            for (auto const& pos: army_symmetric)
                backtrack_count_symmetric_from -= backtrack[pos];
#endif // BACKTRACK

            if (BALANCE >= 0) {
                fill(balance_count_from.begin(), balance_count_from.end(), 0);
                for (auto const&pos: red)
                    ++balance_count_from[pos.parity()];
            }

#endif // BLUE_TO_MOVE

            ArmyPos armyE, armyESymmetric;
#if !VERBOSE
            Image image{blue, red};
#endif // VERBOSE
            // Finally, finally we are going to do the actual moves
            for (int a=0; a<ARMY; ++a) {
                armyE.copy(army, a);
                // The piece that is going to move
                auto const soldier = army[a];
                image.set(soldier, EMPTY);
                armyESymmetric.copy(army_symmetric, mapper.map(soldier));
#if BLUE_TO_MOVE
                auto off_base = off_base_from;
                off_base += tables.base_red(soldier);
                ParityCount parity_count = parity_count_from;
                ++parity_count[soldier.parity()];
                auto edge_count = edge_count_from;
                edge_count -= tables.edge_red(soldier);
#else

# if BACKTRACK
                int backtrack_count = backtrack_count_from + backtrack[soldier];
                int backtrack_count_symmetric = backtrack_count_symmetric_from + backtrack[soldier.symmetric()];
# endif // BACKTRACK

                if (BALANCE >= 0) {
                    balance_count = balance_count_from;
                    --balance_count[soldier.parity()];
                }
#endif // BLUE_TO_MOVE

                // Jumps
                array<Coord, ARMY*2*MOVES+(1+MOVES)> reachable;
                reachable[0] = soldier;
                int nr_reachable = 1;
                if (!CLOSED_LOOP) image.set(soldier, COLORS);
                for (int i=0; i < nr_reachable; ++i) {
                    for (auto move: Coord::moves()) {
                        Coord jumpee{reachable[i], move};
                        if (image.get(jumpee) != RED && image.get(jumpee) != BLUE) continue;
                        Coord target{jumpee, move};
                        if (image.get(target) != EMPTY) continue;
                        image.set(target, COLORS);
                        reachable[nr_reachable++] = target;
                    }
                }
                for (int i=CLOSED_LOOP; i < nr_reachable; ++i)
                    image.set(reachable[i], EMPTY);

                // Slides
                for (auto move: Coord::moves()) {
                    Coord target{soldier, move};
                    if (image.get(target) != EMPTY) continue;
                    reachable[nr_reachable++] = target;
                }

                for (int i=1; i < nr_reachable; ++i) {
                    // armyZ[a] = CoordZ{reachable[i]};
                    if (false) {
                        image.set(reachable[i], RED - BLUE_TO_MOVE);
                        cout << image;
                        image.set(reachable[i], EMPTY);
                    }
                    auto val = reachable[i];

                    Nbits Ndistance_a = Ndistance_army;
                    {
#if BLUE_TO_MOVE
                        Nbits Ndistance_r = Ndistance_red;
                        auto off      = off_base;
                        auto parity_c = parity_count;
                        auto edge_c   = edge_count;

                        if (tables.base_red(val)) {
                            --off;
                            if (off == 0) {
#if !BACKTRACK
                                if (boards_to.solve(red_id, red)) {
                                    image.set(reachable[i], BLUE_TO_MOVE ? BLUE : RED);
                                    cout << "==================================\n";
                                    cout << image << "Solution!" << endl;
                                    image.set(reachable[i], EMPTY);
                                }
#endif // !BACKTRACK
                                goto SOLUTION;
                            }
                        } else {
                            edge_c += tables.edge_red(val);
                            Ndistance_r |=  tables.Ndistance_base_red(val);
                            for (auto const& r: red)
                                Ndistance_a |= tables.Ndistance(val, r);
                        }
                        int distance_red  = __builtin_clz(Ndistance_red);
                        int distance_army = __builtin_clz(Ndistance_a);
                        int pre_moves = min(distance_army / 2, distance_red);
                        --parity_c[val.parity()];
                        int slides = 0;
                        for (auto tc: parity_c)
                            slides += max(tc, 0);
                        int blue_moves = pre_moves + max(slides-pre_moves-edge_c, 0) + off;
                        needed_moves = 2*blue_moves;
#else // BLUE_TO_MOVE
# if BACKTRACK
                        if (backtrack_count - backtrack[val] >= available_moves &&
                            backtrack_count_symmetric - backtrack[val.symmetric()] >= available_moves) continue;
# endif // BACKTRACK

                        if (BALANCE >= 0) {
                            auto balance_c = balance_count;
                            ++balance_c[val.parity()];
                            BalanceMask balance_bits = 0;
                            for (auto b: balance_c)
                                balance_bits |= static_cast<BalanceMask>(1) << b;
                            if (balance_bits & balance_mask) continue;
                        }

                        // We won't notice an increase in army distance, but
                        // these are rare and will be discovered in the late
                        // prune
                        for (auto const& b: blue) {
                            if (tables.base_red(b)) continue;
                            Ndistance_a |= tables.Ndistance(val, b);
                        }
                        int distance_army = __builtin_clz(Ndistance_a);
                        int pre_moves = min((distance_army + 1) / 2, distance_red);
                        int blue_moves = pre_moves + max(slides-pre_moves-edge_count_from, 0) + off_base_from;
                        needed_moves = 2*blue_moves - 1;
#endif // BLUE_TO_MOVE
                        if (needed_moves >= available_moves) {
                            if (VERBOSE) {
                                cout << "   Move " << soldier << " to " << val << "\n";
                                cout << "   Prune " << needed_moves << " > " << available_moves-1 << "\n";
                            }
                            continue;
                        }
                    }
#if BLUE_TO_MOVE
                  SOLUTION:
#endif // BLUE_TO_MOVE
                    armyE.store(val);
                    // cout << "   Final Set pos " << pos << armyE[pos] << "\n";
                    // cout << armyE << "----------------\n";
                    if (CHECK) armyE.check(__LINE__);
                    armyESymmetric.store(val.symmetric());
                    if (CHECK) armyESymmetric.check(__LINE__);
                    int symmetry = cmp(armyE, armyESymmetric);
                    auto moved_id = moved_armies.insert(symmetry >= 0 ? armyE : armyESymmetric);
                    if (CHECK && moved_id == 0)
                        throw(logic_error("Army Insert returns 0"));
#if BLUE_TO_MOVE
                    // The opponent is red and after this it is red's move
                    symmetry *= red_symmetry;
                    if (boards_to.insert(moved_id, red_id, symmetry) && VERBOSE) {
                        // cout << "   symmetry=" << symmetry << "\n   armyE:\n" << armyE << "   armyESymmetric:\n" << armyESymmetric;
                        cout << "   Blue id " << moved_id << "\n" << Image{armyE, red};
                    }
#else  // BLUE_TO_MOVE
                    // The opponent is blue and after this it is blue's move
                    symmetry *= blue_symmetry;
                    if (boards_to.insert(blue_id, moved_id, symmetry) && VERBOSE) {
                        // cout << "   symmetry=" << symmetry << "\n   armyE:\n" << armyE << "   armyESymmetric:\n" << armyESymmetric;
                        cout << "   Red id " << moved_id << "\n" << Image{blue, armyE};
                    }
#endif // BLUE_TO_MOVE
                }

                image.set(soldier, BLUE_TO_MOVE ? BLUE : RED);
            }
        }
    }
    return late;
}
