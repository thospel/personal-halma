NOINLINE
uint64_t NAME(BoardSet& boards_from,
              BoardSet& boards_to,
              ArmyZSet const& moving_armies,
              ArmyZSet const& opponent_armies,
              ArmyZSet& moved_armies,
#if BACKTRACK && !BLUE_TO_MOVE
              BoardTable<uint8_t> const& backtrack,
              BoardTable<uint8_t> const& backtrack_symmetric,
              int solution_moves,
#endif // BACKTRACK && !BLUE_TO_MOVE
              int available_moves) {
#if !BLUE_TO_MOVE
    BalanceMask balance_mask;
    if (balance >= 0) {
        int balance_moves = max(available_moves/2 + balance_delay - ARMY, 0);
        balance_mask = make_balance_mask(balance_min-balance_moves, balance_max+balance_moves);
        balance_mask = ~balance_mask;
    } else
        balance_mask = 0;
    ParityCount balance_count_from, balance_count;
#endif

    uint64_t late = 0;
    while (true) {
        BoardSubSetRef subset{boards_from};

        ArmyId const blue_id = subset.id();
        if (blue_id == 0) break;
        if (VERBOSE) cout << "Processing blue " << blue_id << "\n";
        ArmyZ const& bZ = BLUE_TO_MOVE ?
            moving_armies.at(blue_id) :
            opponent_armies.at(blue_id);
#if BLUE_TO_MOVE
        ArmyZ const bZ_symmetric = bZ.symmetric();
        ArmyPair const blue_pair{bZ, bZ_symmetric};
        if (CHECK) blue_pair.check(__LINE__);
        ArmyMapperPair const b_mapper{blue_pair};
#else  // BLUE_TO_MOVE
        ArmyPair const blue_pair{bZ};
        if (CHECK) blue_pair.check(__LINE__);
        // Will return 0 for symmetric or 1 for asymmetric
        int b_symmetry = blue_pair.symmetry();
#endif  // BLUE_TO_MOVE

        BoardSubSet const& red_armies = subset.armies();
        for (auto const& red_value: red_armies) {
            if (red_value == 0) continue;
            ArmyId red_id;
            auto const symmetry = BoardSubSet::split(red_value, red_id);
            if (VERBOSE) cout << " Sub Processing red " << red_id << "," << symmetry << "\n";
            Army const& blue         = symmetry ? blue_pair.symmetric() : blue_pair.normal();
#if BLUE_TO_MOVE
            ArmyMapper const& mapper = symmetry ? b_mapper.symmetric() : b_mapper.normal();
#else  // BLUE_TO_MOVE
            int  blue_symmetry       = symmetry ? -b_symmetry : b_symmetry;
#endif // BLUE_TO_MOVE

            ArmyZ const& rZ = BLUE_TO_MOVE ?
                opponent_armies.at(red_id) :
                moving_armies.at(red_id);
            Army const red{rZ};
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
                if (b.base_red()) continue;
                ++off_base_from;
                edge_count_from += b.edge_red();
                Ndistance_red |= b.Ndistance_base_red();
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

#if BLUE_TO_MOVE
            int const red_symmetry = rZ.symmetry();
            Army const& army             = blue;
            ArmyZ const& armyZ           = symmetry ? bZ_symmetric : bZ;
            ArmyZ const& armyZ_symmetric = symmetry ? bZ : bZ_symmetric;
#else  // BLUE_TO_MOVE
            Army  const& army            = red;
            ArmyZ const& armyZ           = rZ;
            ArmyZ const armyZ_symmetric  = rZ.symmetric();
            ArmyMapper const mapper{armyZ_symmetric};

#if BACKTRACK
            int backtrack_count_from = 2*ARMY;
            int backtrack_count_symmetric_from = 2*ARMY;
            for (auto const& pos: army) {
                backtrack_count_from -= backtrack[pos];
                backtrack_count_symmetric_from -= backtrack_symmetric[pos];
            }
#endif // BACKTRACK

            if (BALANCE && balance_mask) {
                fill(balance_count_from.begin(), balance_count_from.end(), 0);
                for (auto const&pos: red)
                    ++balance_count_from[pos.parity()];
            }

#endif // BLUE_TO_MOVE

            ArmyZPos armyE, armyESymmetric;
#if !VERBOSE
            Image image{blue, red};
#endif // VERBOSE
            // Finally, finally we are going to do the actual moves
            for (int a=0; a<ARMY; ++a) {
                armyE.copy(armyZ, a);
                // The piece that is going to move
                auto const soldier = army[a];
                image.set(soldier, EMPTY);
                armyESymmetric.copy(armyZ_symmetric, mapper.map(soldier));
#if BLUE_TO_MOVE
                auto off_base = off_base_from;
                off_base += soldier.base_red();
                ParityCount parity_count = parity_count_from;
                ++parity_count[soldier.parity()];
                auto edge_count = edge_count_from;
                edge_count -= soldier.edge_red();
#else

# if BACKTRACK
                int backtrack_count = backtrack_count_from + backtrack[soldier];
                int backtrack_count_symmetric = backtrack_count_symmetric_from + backtrack_symmetric[soldier];
# endif // BACKTRACK

                if (BALANCE && balance_mask) {
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
                        image.set(reachable[i], BLUE_TO_MOVE ? BLUE : RED);
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

                        if (val.base_red()) {
                            --off;
                            if (off == 0) {
#if !BACKTRACK
                                if (boards_to.solve(red_id, rZ)) {
                                    image.set(reachable[i], BLUE_TO_MOVE ? BLUE : RED);
                                    cout << "==================================\n";
                                    cout << image << "Solution!" << endl;
                                    image.set(reachable[i], EMPTY);
                                }
#endif // !BACKTRACK
                                goto SOLUTION;
                            }
                        } else {
                            edge_c += val.edge_red();
                            Ndistance_r |=  val.Ndistance_base_red();
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
                        if (backtrack_count - backtrack[val] >= solution_moves &&
                            backtrack_count_symmetric - backtrack_symmetric[val] >= solution_moves) {
                            if (VERBOSE) {
                                cout << "   Move " << soldier << " to " << val << "\n";
                                cout << "   Prune backtrack: bactrack " << backtrack_count - backtrack[val] << " >= " << solution_moves << " && backtrack_count_symmetric " << backtrack_count_symmetric - backtrack_symmetric[val] << " >= " << solution_moves << "\n";
                            }
                          continue;
                        }
# endif // BACKTRACK

                        if (BALANCE && balance_mask) {
                            auto balance_c = balance_count;
                            ++balance_c[val.parity()];
                            BalanceMask balance_bits = 0;
                            for (auto b: balance_c)
                                balance_bits |= static_cast<BalanceMask>(1) << b;
                            if (balance_bits & balance_mask) {
                                if (VERBOSE) {
                                    cout << "   Move " << soldier << " to " << val << "\n";
                                    cout << "   Prune unbalanced: " << balance_min << " <= " << balance_c << " <= " << balance_max << "\n";
                                }
                                continue;
                            }
                        }

                        // We won't notice an increase in army distance, but
                        // these are rare and will be discovered in the late
                        // prune
                        for (auto const& b: blue) {
                            if (b.base_red()) continue;
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
                                cout << "   Prune game length: " << needed_moves << " > " << available_moves-1 << "\n";
                            }
                            continue;
                        }
                    }
#if BLUE_TO_MOVE
                  SOLUTION:
#endif // BLUE_TO_MOVE
                    CoordZ valZ{val};
                    armyE.store(valZ);
                    // cout << "   Final Set pos " << pos << armyE[pos] << "\n";
                    // cout << armyE << "----------------\n";
                    if (CHECK) armyE.check(__LINE__);
                    armyESymmetric.store(valZ.symmetric());
                    if (CHECK) armyESymmetric.check(__LINE__);
                    int result_symmetry = cmp(armyE, armyESymmetric);
                    auto moved_id = moved_armies.insert(result_symmetry >= 0 ? armyE : armyESymmetric);
                    if (CHECK && moved_id == 0)
                        throw(logic_error("ArmyZ Insert returns 0"));
#if BLUE_TO_MOVE
                    // The opponent is red and after this it is red's move
                    result_symmetry *= red_symmetry;
                    if (boards_to.insert(moved_id, red_id, result_symmetry) && VERBOSE) {
                        // cout << "   symmetry=" << result_symmetry << "\n   armyE:\n" << armyE << "   armyESymmetric:\n" << armyESymmetric;
                        image.set(val, BLUE);
                        cout << "   Inserted Blue id " << moved_id << "\n" << image;
                        image.set(val, EMPTY);
                    }
#else  // BLUE_TO_MOVE
                    // The opponent is blue and after this it is blue's move
                    result_symmetry *= blue_symmetry;
                    if (boards_to.insert(blue_id, moved_id, result_symmetry) && VERBOSE) {
                        // cout << "   symmetry=" << result_symmetry << "\n   armyE:\n" << armyE << "   armyESymmetric:\n" << armyESymmetric;
                        image.set(val, RED);
                        cout << "   Inserted Red id " << moved_id << "\n" << image;
                        image.set(val, EMPTY);
                    }
#endif // BLUE_TO_MOVE
                }

                image.set(soldier, BLUE_TO_MOVE ? BLUE : RED);
            }
        }
    }
    return late;
}
