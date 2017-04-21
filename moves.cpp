#include <future>

#if BLUE_TO_MOVE
# define COLOR_TO_MOVE	blue
#else  // BLUE_TO_MOVE
# define COLOR_TO_MOVE	red
#endif // BLUE_TO_MOVE

#if BACKTRACK
# define _BACKTRACK _backtrack
#else  // BACKTRACK
# define _BACKTRACK
#endif // BACKTRACK

#if RED_BUILDER
# define RED_NORMAL	(!BLUE_TO_MOVE && !BACKTRACK)
# define BLUE_NORMAL	( BLUE_TO_MOVE && !BACKTRACK)
#else  // RED_BUILDER
# define RED_NORMAL	0
# define BLUE_NORMAL	0
#endif // RED_BUILDER

#if SLOW
# define _SLOW _slow
#else  // SLOW
# define _SLOW _fast
#endif // SLOW

#define VERBOSE (SLOW && UNLIKELY(verbose))

#define NAME	CAT(thread_,CAT(COLOR_TO_MOVE,CAT(_moves,CAT(_BACKTRACK,_SLOW))))

NOINLINE
Statistics NAME(uint thid,
                BoardSet& boards_from,
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
    tid = thid;
    signal_generation_seen = signal_generation.load(memory_order_relaxed);
    // logger << "Started (Set " << available_moves << ")\n" << flush;
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

#if RED_NORMAL
    BoardSubSetRedBuilder subset_to;
#endif // RED_NORMAL
    Statistics stats;
    while (true) {
#if BLUE_NORMAL
        BoardSubSetRedRef subset_from{boards_from};
#else // BLUE_NORMAL
        BoardSubSetRef subset_from{boards_from};
#endif // BLUE_NORMAL

        ArmyId const blue_id = subset_from.id();
        if (blue_id == 0) break;
        if (VERBOSE) logger << "Processing blue " << blue_id << "\n" << flush;

        uint signal_gen = signal_generation.load(memory_order_relaxed);
        if (UNLIKELY(signal_generation_seen != signal_gen)) {
            signal_generation_seen = signal_gen;
            logger << time_string() << ": Processing blue " << setw(6) << blue_id << "," << setw(9) << boards_from.size() + subset_from.armies().size() << " boards left\n" << flush;
            if (signal_gen % 2 == 0) {
                logger << "Forced exit" << endl;
                break;
            }
        }

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

        Nbits Ndistance_red = NLEFT >> tables.infinity();
        int off_base_from   = 0;
        int edge_count_from = 0;
        ParityCount parity_blue = tables.parity_count();
        for (auto const& b: blue_pair.normal()) {
            --parity_blue[b.parity()];
            if (b.base_red()) continue;
            ++off_base_from;
            edge_count_from += b.edge_red();
            Ndistance_red |= b.Ndistance_base_red();
        }
        int const distance_red = __builtin_clz(Ndistance_red);
#if BLUE_TO_MOVE
        ParityCount const parity_blue_symmetric = {{
            parity_blue[0],
            parity_blue[2],
            parity_blue[1],
            parity_blue[3],
        }};
#endif // BLUE_TO_MOVE
        int const slides = min_slides(parity_blue);

#if !BLUE_TO_MOVE && !RED_NORMAL
        BoardSubSet subset_to;
        subset_to.create();
#endif // !BLUE_TO_MOVE && !RED_NORMAL

        auto const& red_armies = subset_from.armies();
        for (auto const& red_value: red_armies) {
            if (!BLUE_NORMAL && red_value == 0) continue;
            ArmyId red_id;
            auto const symmetry = BoardSubSet::split(red_value, red_id);
            if (VERBOSE) logger << " Sub Processing red " << red_id << "," << symmetry << "\n" << flush;
            Army const& blue =
                symmetry ? blue_pair.symmetric() : blue_pair.normal();
#if BLUE_TO_MOVE
            ParityCount const& parity_count_from =
                symmetry ? parity_blue_symmetric : parity_blue;
            ArmyMapper const& mapper =
                symmetry ? b_mapper.symmetric() : b_mapper.normal();
#else  // BLUE_TO_MOVE
            int  blue_symmetry =
                symmetry ? -b_symmetry : b_symmetry;
#endif // BLUE_TO_MOVE

            ArmyZ const& rZ = BLUE_TO_MOVE ?
                opponent_armies.at(red_id) :
                moving_armies.at(red_id);
            Army const red{rZ};
            if (CHECK) red.check(__LINE__);

            Image image{blue, red};
            if (VERBOSE)
                logger << "  From: [" << blue_id << ", " << red_id << ", " << symmetry << "] " << available_moves << " moves\n" << image << flush;

            Nbits Ndistance_army = NLEFT >> tables.infinity();
            for (auto const& b: blue) {
                if (b.base_red()) continue;
                for (auto const& r: red)
                    Ndistance_army |= tables.Ndistance(r, b);
            }
            int const distance_army = __builtin_clz(Ndistance_army);

            if (VERBOSE) {
                logger << "  Slides >= " << slides << ", red edge count " << edge_count_from << "\n";
                logger << "  Distance army=" << distance_army << "\n";
                logger << "  Distance red =" << distance_red  << "\n";
                logger << "  Off base=" << off_base_from << "\n" << flush;
            }
            int pre_moves = min((distance_army + BLUE_TO_MOVE) / 2, distance_red);
            int blue_moves = pre_moves + max(slides-pre_moves-edge_count_from, 0) + off_base_from;
            int needed_moves = 2*blue_moves - BLUE_TO_MOVE;
            if (VERBOSE)
                logger << "  Needed moves=" << static_cast<int>(needed_moves) << "\n" << flush;
            if (needed_moves > available_moves) {
                if (VERBOSE)
                    logger << "  Late prune " << needed_moves << " > " << available_moves << "\n" << flush;
                stats.late_prune();
                continue;
            }

            // jump_only indicates that all blue moves must now be jumps
            bool const jump_only = available_moves <= off_base_from*2 && !slides;
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
            for (auto const& pos: red) {
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
#if BLUE_TO_MOVE
            array<Coord, X*Y-2*ARMY+1> reachable;
#else  // BLUE_TO_MOVE
            array<Coord, X*Y-2*ARMY+1+ARMY+1> reachable;
            uint red_top_from = 0;
            if (jump_only) {
                red_top_from = reachable.size()-1;
                for (auto const& r: tables.army_red()) {
                    reachable[red_top_from] = r;
                    red_top_from -= image.get(r) == EMPTY;
                }
            }
#endif // BLUE_TO_MOVE

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
                uint red_top = 0;
                if (jump_only) {
                    reachable[red_top_from] = soldier;
                    red_top = red_top_from - soldier.base_red();
                }

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
                int nr_reachable = 1;
                if (!(BLUE_TO_MOVE && jump_only) || !soldier.base_red()) {
                    reachable[0] = soldier;
                    if (!CLOSED_LOOP) image.set(soldier, COLORS);
                    for (int i=0; i < nr_reachable; ++i) {
                        for (auto move: Coord::directions()) {
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

                    // Only allow jumps off base...
                    if ((BLUE_TO_MOVE && jump_only) ||
                        (BLUE_TO_MOVE && prune_jump && !soldier.base_blue())) {
                        // ... or jump onto target
                        int i = 1;
                        while (i < nr_reachable) {
                            auto const val = reachable[i];
                            // Maybe allow jumping to the red edge too ?
                            if (val.base_red()) ++i;
                            else {
                                reachable[i] = reachable[--nr_reachable];
                                if (VERBOSE) {
                                    logger << "   Move " << soldier << " to " << val << "\n";
                                    logger << "   Prune blue jump target outside of red base\n";
                                    logger.flush();
                                }
                            }
                        }
                    }
                }

                // Slides
                if (BLUE_TO_MOVE && jump_only) {
                    if (VERBOSE)
                        logger << "   Prune all blue slides (jump only)\n" << flush;
                } else if (BLUE_TO_MOVE && prune_slide) {
                    if (slides) {
                        // Either slide off base...
                        if (soldier.base_blue()) goto NORMAL_SLIDES;
                        // ... or slide onto target
                        for (auto move: Coord::directions()) {
                            Coord target{soldier, move};
                            if (image.get(target) != EMPTY) continue;
                            if (!target.base_red()) {
                                if (VERBOSE) {
                                    logger << "   Move " << soldier << " to " << target << "\n";
                                    logger << "   Prune blue slide target outside of red base\n" << flush;
                                }
                                continue;
                            }
                            reachable[nr_reachable++] = target;
                        }
                    } else if (VERBOSE) {
                        logger << "   Prune all blue sides (target parity reached)\n" << flush;
                    }
                } else {
                  NORMAL_SLIDES:
                    for (auto move: Coord::directions()) {
                        Coord target{soldier, move};
                        if (image.get(target) != EMPTY) continue;
                        reachable[nr_reachable++] = target;
                    }
                }

                for (int i=1; i < nr_reachable; ++i) {
                    // armyZ[a] = CoordZ{reachable[i]};
                    auto const val = reachable[i];
                    if (false) {
                        image.set(val, BLUE_TO_MOVE ? BLUE : RED);
                        logger << image << flush;
                        image.set(val, EMPTY);
                    }
#if BLUE_TO_MOVE
                    int edge_c;
#endif // BLUE_TO_MOVE

                    Nbits Ndistance_a = Ndistance_army;
                    {
#if BLUE_TO_MOVE
                        Nbits Ndistance_r = Ndistance_red;
                        auto off      = off_base;
                        auto parity_c = parity_count;
                        edge_c        = edge_count;

                        if (val.base_red()) {
                            --off;
                            if (off == 0) {
#if !BACKTRACK
                                if (boards_to.solve(red_id, rZ)) {
                                    image.set(val, BLUE_TO_MOVE ? BLUE : RED);
                                    logger << "==================================\n";
                                    logger << image << "Solution!" << endl;
                                    image.set(val, EMPTY);
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
                        int const distance_red  = __builtin_clz(Ndistance_r);
                        int const distance_army = __builtin_clz(Ndistance_a);
                        int const pre_moves = min(distance_army / 2, distance_red);
                        --parity_c[val.parity()];
                        int const slides = min_slides(parity_c);
                        int blue_moves = pre_moves + max(slides-pre_moves-edge_c, 0) + off;
                        needed_moves = 2*blue_moves;
#else // BLUE_TO_MOVE
# if BACKTRACK
                        if (backtrack_count           - backtrack[val]           >= solution_moves &&
                            backtrack_count_symmetric - backtrack_symmetric[val] >= solution_moves) {
                            if (VERBOSE) {
                                logger << "   Move " << soldier << " to " << val << "\n";
                                logger << "   Prune backtrack: bactrack " << backtrack_count - backtrack[val] << " >= " << solution_moves << " && backtrack_count_symmetric " << backtrack_count_symmetric - backtrack_symmetric[val] << " >= " << solution_moves << "\n";
                                logger.flush();
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
                                    logger << "   Move " << soldier << " to " << val << "\n";
                                    logger << "   Prune unbalanced: " << balance_min << " <= " << balance_c << " <= " << balance_max << "\n";
                                    logger.flush();
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
                                logger << "   Move " << soldier << " to " << val << "\n";
                                logger << "   Prune game length: " << needed_moves << " > " << available_moves-1 << " da=" << distance_army << ", dr=" << distance_red << ", pm=" << pre_moves << ", sl=" << slides << ", bm=" << blue_moves << "\n";
                                logger.flush();
                            }
                            continue;
                        }
                    }
#if BLUE_TO_MOVE
                  SOLUTION:
#else  // BLUE_TO_MOVE
                    if (jump_only) {
                        uint r_top = red_top;
                        for (uint i = reachable.size()-1; i > r_top; --i)
                            image.set(reachable[i], COLORS);
                        // Setting pos val must be able to override COLORS
                        image.set(val, RED);
                        for (uint i = reachable.size()-1; i > r_top; --i) {
                            for (auto move: Coord::directions()) {
                                Coord jumpee{reachable[i], move};
                                if (image.get(jumpee) != RED && image.get(jumpee) != BLUE) continue;
                                Coord target{jumpee, move};
                                Color c = image.get(target);
                                if (c & RED) continue;
                                if (c == BLUE) {
                                    if (!target.base_red()) goto ACCEPTABLE;
                                    continue;
                                }
                                image.set(target, COLORS);
                                reachable[r_top--] = target;
                            }
                        }
                        for (uint i = reachable.size()-1; i > r_top; --i)
                            image.set(reachable[i], EMPTY);
                        image.set(val, EMPTY);
                        if (VERBOSE) {
                            logger << "   Move " << soldier << " to " << val << "\n";
                            logger << "   Prune blue cannot jump to target\n";
                            logger.flush();
                        }
                        continue;
                      ACCEPTABLE:
                        for (uint i = reachable.size()-1; i > r_top; --i)
                            image.set(reachable[i], EMPTY);
                        image.set(val, EMPTY);
                    }
#endif // BLUE_TO_MOVE
                    CoordZ valZ{val};
                    armyE.store(valZ);
                    // logger << "   Final Set pos " << pos << armyE[pos] << "\n";
                    // logger << armyE << "----------------\n";
                    // logger.flush();
                    if (CHECK) armyE.check(__LINE__);
                    armyESymmetric.store(valZ.symmetric());
                    if (CHECK) armyESymmetric.check(__LINE__);
                    int result_symmetry = cmp(armyE, armyESymmetric);
                    auto moved_id = moved_armies.insert(result_symmetry >= 0 ? armyE : armyESymmetric, stats);
                    if (CHECK && moved_id == 0)
                        throw(logic_error("ArmyZ Insert returns 0"));
#if BLUE_TO_MOVE
                    // The opponent is red and after this it is red's move
                    result_symmetry *= red_symmetry;
                    if (boards_to.insert(moved_id, red_id, result_symmetry, stats)) {
                        if (edge_c) stats.edge();
                        if (VERBOSE) {
                            // logger << "   symmetry=" << result_symmetry << "\n   armyE:\n" << armyE << "   armyESymmetric:\n" << armyESymmetric;
                            image.set(val, BLUE);
                            logger << "   Inserted Blue id " << moved_id << "\n" << image << flush;
                            image.set(val, EMPTY);
                        }
                    }
#else  // BLUE_TO_MOVE
                    // The opponent is blue and after this it is blue's move
                    result_symmetry *= blue_symmetry;
                    if (subset_to.insert(moved_id, result_symmetry, stats)) {
                        if (VERBOSE) {
                            // logger << "   symmetry=" << result_symmetry << "\n   armyE:\n" << armyE << "   armyESymmetric:\n" << armyESymmetric;
                            image.set(val, RED);
                            logger << "   Inserted Red id " << moved_id << "\n" << image << flush;
                            image.set(val, EMPTY);
                        }
                    }
#endif // BLUE_TO_MOVE
                }

                image.set(soldier, BLUE_TO_MOVE ? BLUE : RED);
            }
        }
#if !BLUE_TO_MOVE
        if (edge_count_from) stats.edge(subset_to.size());
        boards_to.insert(blue_id, subset_to);
#endif // !BLUE_TO_MOVE

    }
    // logger << "Stopped (Set " << available_moves << ")\n" << flush;
    return stats;
}

#if !BLUE_TO_MOVE
# define ALL_NAME CAT(make_all_moves,CAT(_BACKTRACK,_SLOW))
# define BLUE_MOVES_BACKTRACK CAT(thread_blue_moves_backtrack,_SLOW)
# define BLUE_MOVES           CAT(thread_blue_moves,_SLOW)
# define RED_MOVES_BACKTRACK  CAT(thread_red_moves_backtrack,_SLOW)
# define RED_MOVES            CAT(thread_red_moves,_SLOW)

StatisticsE ALL_NAME(BoardSet& boards_from,
                     BoardSet& boards_to,
                     ArmyZSet const& moving_armies,
                     ArmyZSet const& opponent_armies,
                     ArmyZSet& moved_armies,
#if BACKTRACK
                     int solution_moves,
                     BoardTable<uint8_t> const& red_backtrack,
                     BoardTable<uint8_t> const& red_backtrack_symmetric,
#endif // BACKTRACK
                     int nr_moves) {
    StatisticsE stats{nr_moves, opponent_armies.size()};
    stats.start();
    stats.armyset_untry(moved_armies.size());
    stats.boardset_untry(boards_to.size());

    vector<future<Statistics>> results;
    int blue_to_move = nr_moves & 1;
    if (blue_to_move) {
#if BACKTRACK
        if (solution_moves > 0) {
            for (uint i=1; i < nr_threads; ++i)
                results.emplace_back
                    (async
                     (launch::async, BLUE_MOVES_BACKTRACK,
                      i, ref(boards_from), ref(boards_to),
                      ref(moving_armies), ref(opponent_armies), ref(moved_armies),
                      nr_moves));
            static_cast<Statistics&>(stats) = BLUE_MOVES_BACKTRACK
                (0, boards_from, boards_to,
                 moving_armies, opponent_armies, moved_armies,
                 nr_moves);
        } else
#endif // BACKTRACK
            {
                for (uint i=1; i < nr_threads; ++i)
                    results.emplace_back
                        (async
                         (launch::async, BLUE_MOVES,
                          i, ref(boards_from), ref(boards_to),
                          ref(moving_armies), ref(opponent_armies), ref(moved_armies),
                          nr_moves));
                static_cast<Statistics&>(stats) = BLUE_MOVES
                    (0, boards_from, boards_to,
                     moving_armies, opponent_armies, moved_armies,
                     nr_moves);
            }
    } else {
#if BACKTRACK
        if (solution_moves > 0) {
            for (uint i=1; i < nr_threads; ++i)
                results.emplace_back
                    (async
                     (launch::async, RED_MOVES_BACKTRACK,
                      i, ref(boards_from), ref(boards_to),
                      ref(moving_armies), ref(opponent_armies), ref(moved_armies),
                      ref(red_backtrack), ref(red_backtrack_symmetric),
                      solution_moves, nr_moves));
            static_cast<Statistics&>(stats) = RED_MOVES_BACKTRACK
                (0, boards_from, boards_to,
                 moving_armies, opponent_armies, moved_armies,
                 red_backtrack, red_backtrack_symmetric,
                 solution_moves, nr_moves);
        } else
#endif // BACKTRACK
            {
                for (uint i=1; i < nr_threads; ++i)
                    results.emplace_back
                        (async
                         (launch::async, RED_MOVES,
                          i, ref(boards_from), ref(boards_to),
                          ref(moving_armies), ref(opponent_armies), ref(moved_armies),
                          nr_moves));
                static_cast<Statistics&>(stats) = RED_MOVES
                    (0, boards_from, boards_to,
                     moving_armies, opponent_armies, moved_armies,
                     nr_moves);
            }
    }
    for (auto& result: results) stats += result.get();
    stats.armyset_size(moved_armies.size());
    stats.boardset_size(boards_to.size());
    stats.stop();

    return stats;
}

# undef ALL_NAME
# undef BLUE_MOVES_BACKTRACK
# undef BLUE_MOVES
# undef RED_MOVES_BACKTRACK
# undef RED_MOVES
#endif // BLUE_TO_MOVE

#undef COLOR_TO_MOVE
#undef NAME
#undef _BACKTRACK
#undef _SLOW
#undef VERBOSE
