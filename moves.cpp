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

#if SLOW
# define _SLOW _slow
#else  // SLOW
# define _SLOW _fast
#endif // SLOW

#define GUARD_VAR		CAT(dummy_, __LINE__)
#define START_GUARD(code)	for (uint GUARD_VAR=0; GUARD_VAR < 1; ++GUARD_VAR, (code)) {
#define END_GUARD		}

#define VERBOSE (SLOW && UNLIKELY(verbose))

#define NAME	CAT(thread_,CAT(COLOR_TO_MOVE,CAT(_moves,CAT(_BACKTRACK,_SLOW))))

NOINLINE
Statistics NAME(uint thid,
#if BLUE_TO_MOVE
                BoardSetRed& boards_from,
                BoardSetBlue& boards_to,
#else // BLUE_TO_MOVE
                BoardSetBlue& boards_from,
                BoardSetRed& boards_to,
#endif // BLUE_TO_MOVE
                ArmySet const& moving_armies,
                ArmySet const& opponent_armies,
                ArmySet& moved_armies,
#if BACKTRACK && !BLUE_TO_MOVE
                BoardTable<uint8_t> const& backtrack,
                BoardTable<uint8_t> const& backtrack_symmetric,
                int solution_moves,
#endif // BACKTRACK && !BLUE_TO_MOVE
                int available_moves) HOT;
Statistics NAME(uint thid,
#if BLUE_TO_MOVE
                BoardSetRed& boards_from,
                BoardSetBlue& boards_to,
#else // BLUE_TO_MOVE
                BoardSetBlue& boards_from,
                BoardSetRed& boards_to,
#endif // BLUE_TO_MOVE
                ArmySet const& moving_armies,
                ArmySet const& opponent_armies,
                ArmySet& moved_armies,
#if BACKTRACK && !BLUE_TO_MOVE
                BoardTable<uint8_t> const& backtrack,
                BoardTable<uint8_t> const& backtrack_symmetric,
                int solution_moves,
#endif // BACKTRACK && !BLUE_TO_MOVE
                int available_moves) {
    tid = thid;
    ThreadData thread_data;
    // logger << "Started (Set " << available_moves << ")\n" << flush;
#if !BLUE_TO_MOVE
    BalanceMask balance_mask;
    if (balance >= 0) {
        int balance_moves = max(available_moves/2 + balance_delay - static_cast<int>(ARMY), 0);
        balance_mask = make_balance_mask(balance_min-balance_moves, balance_max+balance_moves);
        balance_mask = ~balance_mask;
    } else
        balance_mask = 0;
    ParityCount balance_count_from, balance_count;
#endif

    Statistics stats;
    Image image_normal, image_symmetric;
#if !BLUE_TO_MOVE
    BoardSubsetRedBuilder& subset_to = boards_to.builder();
#endif // !BLUE_TO_MOVE
    while (true) {
#if BLUE_TO_MOVE
        BoardSubsetRedRef subset_from{boards_from};
#else // BLUE_TO_MOVE
        BoardSubsetBlueRef subset_from{boards_from};
#endif // BLUE_TO_MOVE

        ArmyId const blue_id = subset_from.id();
        if (blue_id == 0) break;
        if (VERBOSE) logger << "Processing blue " << blue_id << "\n" << flush;

        if (thread_data.signalled()) {
            if (tid == 0) {
                logger << time_string() << ": Processing blue " << setw(6) << blue_id << " (" << get_memory() / 1000000 << " MB)\n" << setw(10) << boards_from.size() + subset_from.armies().size() << " boards ->  " << setw(10) << boards_to.size() << " boards, " << setw(9) << moved_armies.size() << " armies\n";
                if (MEMORY_REPORT) {
                    if (STATISTICS && !BLUE_TO_MOVE)
                        logger << "Largest subset: " << stats.largest_subset() << "\n";
                    memory_report(logger,
                                  moving_armies, opponent_armies, moved_armies,
                                  boards_from, boards_to);
                }
                logger.flush();
            }
            if (thread_data.is_terminated()) {
                logger << "Forced exit" << endl;
                break;
            }
        }

        Army const bZ{BLUE_TO_MOVE ?
                moving_armies.cat(blue_id) :
                opponent_armies.cat(blue_id)};
        if (CHECK) bZ.check(__FILE__, __LINE__);
        Army const bZ_symmetric{bZ, SYMMETRIC};
        if (CHECK) bZ_symmetric.check(__FILE__, __LINE__);
#if BLUE_TO_MOVE
        ArmyMapperPair const b_mapper{bZ, bZ_symmetric};
#else  // BLUE_TO_MOVE
        // Will be 0 for symmetric or 1 for asymmetric
        int b_symmetry = !SYMMETRY ? 0 :
            bZ == bZ_symmetric ? 0 : 1;
#endif  // BLUE_TO_MOVE
        image_normal.set(bZ, BLUE);
        START_GUARD(image_normal.set(bZ, EMPTY));
        image_symmetric.set(bZ_symmetric, BLUE);
        START_GUARD(image_symmetric.set(bZ_symmetric, EMPTY));

        Nbits Ndistance_red = tables.Ninfinity();
        int off_base_from = ARMY;
        int edge_count_from = 0;
        ParityCount parity_blue = tables.parity_count();
        for (auto const b: bZ) {
            --parity_blue[b.parity()];
            off_base_from -= b.base_red();
            edge_count_from += b.edge_red();
            // No need to check for !b.base_red() since Ndistance_base_red()
            // has ignoring soldiers on the red base builtin
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
        // All blue moves must now be from off base to on_base:
        bool const blue_base_only = available_moves <= off_base_from*2;
        // All blue moves must now be jumps from off base to on_base:
        bool const blue_jump_only = blue_base_only && !slides;

#if !BLUE_TO_MOVE
        uint non_blue = tables.nr_deep_red();
        if (blue_jump_only) {
            int i = non_blue;
            while (--i >= 0) {
                Coord const pos = tables.deep_red_base(i);
                non_blue -= image_normal.get(pos) == BLUE;
            }
        }
# if BACKTRACK
        subset_from.sort_compress();
# else // BACKTRACK
        subset_from.sort();
        ArmyId red_value_previous = 0;
# endif // BACKTRACK
#endif // BLUE_TO_MOVE
        auto const& red_armies = subset_from.armies();
        for (auto const red_value: red_armies) {
#if !BLUE_TO_MOVE
# if BACKTRACK
            stats.boardset_unique();
# else // BACKTRACK
            if (red_value == red_value_previous) continue;
            stats.boardset_unique();
            red_value_previous = red_value;
# endif // BACKTRACK
#endif // !BLUE_TO_MOVE
            ArmyId red_id;
            auto const symmetry = BoardSubsetBlue::split(red_value, red_id);
            if (VERBOSE) logger << " Sub Processing red " << red_id << "," << symmetry << "\n" << flush;
            Army const& blue = symmetry ? bZ_symmetric : bZ;
#if BLUE_TO_MOVE
            ParityCount const& parity_count_from =
                symmetry ? parity_blue_symmetric : parity_blue;
            ArmyMapper const& mapper =
                symmetry ? b_mapper.symmetric() : b_mapper.normal();
#else  // BLUE_TO_MOVE
            int  blue_symmetry =
                symmetry ? -b_symmetry : b_symmetry;
#endif // BLUE_TO_MOVE

            Army const red{BLUE_TO_MOVE ?
                    opponent_armies.cat(red_id) :
                    moving_armies.cat(red_id)};
            if (CHECK) red.check(__FILE__, __LINE__);

            auto& image = symmetry ? image_symmetric : image_normal;
            image.set(red, RED);
            START_GUARD(image.set(red, EMPTY))
            if (VERBOSE)
                logger << "  From: [" << blue_id << ", " << red_id << ", " << symmetry << "] " << available_moves << " moves\n" << image << flush;

            Nbits Ndistance_army = tables.Ninfinity();
            for (auto const b: blue) {
                if (b.base_red()) continue;
                for (auto const r: red)
                    Ndistance_army |= tables.Ndistance(b, r);
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

#if BLUE_TO_MOVE
            int const red_symmetry = red.symmetry();
            Army const& army           = blue;
            Army const& army_symmetric = symmetry ? bZ : bZ_symmetric;

            array<Coord, MAX_X*MAX_Y/4+1> reachable;
            uint red_empty = 0;
            if (available_moves <= off_base_from*2+3) {
                for (uint i=tables.nr_deep_red(); i<ARMY; ++i) {
                    Coord const pos = tables.deep_red_base(i);
                    red_empty += image.is_EMPTY(pos);
                }
            }
#else  // BLUE_TO_MOVE
            Army const& army           = red;
            Army const  army_symmetric{red, SYMMETRIC};
            if (CHECK) army_symmetric.check(__FILE__, __LINE__);
            ArmyMapper const mapper{army_symmetric};

#if BACKTRACK
            int backtrack_count_from = 2*ARMY;
            int backtrack_count_symmetric_from = 2*ARMY;
            for (auto const pos: red) {
                backtrack_count_from -= backtrack[pos];
                backtrack_count_symmetric_from -= backtrack_symmetric[pos];
            }
#endif // BACKTRACK

            if (BALANCE && balance_mask) {
                std::memset(&balance_count_from[0], 0, sizeof(balance_count_from));
                for (auto const pos: red)
                    ++balance_count_from[pos.parity()];
            }

            array<Coord, MAX_X*MAX_Y/4+1+MAX_ARMY+1> reachable;
            uint red_top_from = 0;
            uint deep_red_empty = 2;
            if (blue_base_only) {
                // Find EMPTY in the shallow red base
                red_top_from = reachable.size()-1;
                // Walk shallow red positions
                for (uint i = tables.nr_deep_red(); i < ARMY; ++i) {
                    Coord const r = tables.deep_red_base(i);
                    reachable[red_top_from] = r;
                    red_top_from -= image.is_EMPTY(r);
                }
                if (non_blue) {
                    deep_red_empty = 0;
                    int i = tables.nr_deep_red();
                    while (--i >= 0) {
                        Coord const r = tables.deep_red_base(i);
                        deep_red_empty += image.is_EMPTY(r);
                    }
                }
            }
#endif // BLUE_TO_MOVE

            // Finally, finally we are going to do the actual moves
            ArmyPos armyE, armyESymmetric;
            for (uint a=0; a<ARMY; ++a) {
                armyE.copy(army, a);
                if (CHECK) armyE.check(__FILE__, __LINE__);
                // The piece that is going to move
                auto const soldier = army[a];
                armyESymmetric.copy(army_symmetric, mapper.map(soldier));
                if (CHECK) armyESymmetric.check(__FILE__, __LINE__);
#if BLUE_TO_MOVE
                auto off_base = off_base_from;
                off_base += soldier.base_red();
                ParityCount parity_count = parity_count_from;
                ++parity_count[soldier.parity()];
                auto edge_count = edge_count_from;
                edge_count -= soldier.edge_red();
#else
                uint red_top = 0;
                if (blue_base_only) {
                    reachable[red_top_from] = soldier;
                    red_top = red_top_from - soldier.shallow_red();
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
                if (!(BLUE_TO_MOVE && blue_jump_only) || !soldier.base_red()) {
                    reachable[0] = soldier;
                    image.set(soldier, CLOSED_LOOP ? EMPTY : COLORS);
                    for (int i=0; i < nr_reachable; ++i) {
                        auto jumpees      = reachable[i].jumpees();
                        auto jump_targets = reachable[i].jump_targets();
                        for (uint r = 0; r < RULES; ++r, jumpees.next(), jump_targets.next()) {
                            auto const jumpee = jumpees.current();
                            auto const target = jump_targets.current();
                            if (!image.jumpable(jumpee, target)) continue;
                            image.set(target, COLORS);
                            reachable[nr_reachable++] = target;
                        }
                    }
                    for (int i=1; i < nr_reachable; ++i)
                        image.set(reachable[i], EMPTY);
                    image.set(soldier, BLUE_TO_MOVE ? BLUE : RED);

                    // Only allow jumps off base...
                    if ((BLUE_TO_MOVE && blue_jump_only) ||
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
                if (BLUE_TO_MOVE && blue_jump_only) {
                    if (VERBOSE)
                        logger << "   Prune all blue slides (jump only)\n" << flush;
                } else if (BLUE_TO_MOVE && prune_slide) {
                    if (slides) {
                        // Either slide off base...
                        if (soldier.base_blue()) goto NORMAL_SLIDES;
                        // ... or slide onto target
                        auto slide_targets = soldier.slide_targets();
                        for (uint r = 0; r < RULES; ++r, slide_targets.next()) {
                            auto const target = slide_targets.current();
                            if (!target.base_red()) {
                                if (VERBOSE) {
                                    logger << "   Move " << soldier << " to " << target << "\n";
                                    logger << "   Prune blue slide target outside of red base\n" << flush;
                                }
                                continue;
                            }
                            if (image.get(target) != EMPTY) continue;
                            reachable[nr_reachable++] = target;
                        }
                    } else if (VERBOSE) {
                        logger << "   Prune all blue sides (target parity reached)\n" << flush;
                    }
                } else {
                  NORMAL_SLIDES:
                    auto slide_targets = soldier.slide_targets();
                    for (uint r = 0; r < RULES; ++r, slide_targets.next()) {
                        auto const target = slide_targets.current();
                        if (image.get(target) != EMPTY) continue;
                        reachable[nr_reachable++] = target;
                    }
                }

                for (int i=1; i < nr_reachable; ++i) {
                    // army[a] = Coord{reachable[i]};
                    auto const val = reachable[i];
                    if (false)
                        logger << image.str(soldier, val, BLUE_TO_MOVE ? BLUE : RED) << flush;
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
                                if (!BACKTRACK &&
                                    boards_to.solve(red_id, red)) {
                                    logger << "==================================\n";
                                    logger << image.str(soldier, val, BLUE_TO_MOVE ? BLUE : RED) << "Solution!" << endl;
                                }
                                goto SOLUTION;
                            }
                        } else {
                            edge_c += val.edge_red();
                            Ndistance_r |=  val.Ndistance_base_red();
                            for (auto const r: red)
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
                        for (auto const b: blue) {
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
#if BLUE_TO_MOVE
                        if (available_moves <= off*2+1) {
                            image.set(soldier, EMPTY);
                            image.set(val, BLUE);
                            if (red_empty + soldier.shallow_red() - val.shallow_red() == 0) {
                                // The shallow red base is full. Can any red
                                // soldier leave in such a way that blue is able
                                // to fill the hole?
                                // logger << "Full:\n" << image.str(soldier, val, BLUE);
                                array<Coord, MAX_X*MAX_Y/4+1> reachable;
                                uint nr_reachable = 0;
                                for (uint i=0; i<a; ++i) {
                                    // army == blue
                                    auto b = army[i];
                                    reachable[nr_reachable] = b;
                                    nr_reachable += 1-b.base_red();
                                }
                                for (uint i=a+1; i<ARMY; ++i) {
                                    // army == blue
                                    auto b = army[i];
                                    reachable[nr_reachable] = b;
                                    nr_reachable += 1-b.base_red();
                                }
                                reachable[nr_reachable] = val;
                                nr_reachable += 1-val.base_red();
                                uint blues = nr_reachable;

                                if (slides)
                                    // We could preprocess this quite a bit
                                    for (uint i = 0; i < nr_reachable; ++i) {
                                        Coord const pos = reachable[i];
                                        if (!pos.edge_red()) continue;
                                        auto targets = pos.slide_targets();
                                        for (uint r = 0; r < RULES; ++r, targets.next()) {
                                            auto const target = targets.current();
                                            if (!target.shallow_red()) continue;
                                            if (image.get(target) == RED)
                                                goto SLIDABLE;
                                        }
                                    }

                                // BLUE cannot slide onto the base. How about jumps?
                                // Consider all places BLUE can reach by jumping
                                for (uint i = 0; i < nr_reachable; ++i) {
                                    auto jumpees      = reachable[i].jumpees();
                                    auto jump_targets = reachable[i].jump_targets();
                                    for (uint r = 0; r < RULES; ++r, jumpees.next(), jump_targets.next()) {
                                        auto const jumpee = jumpees.current();
                                        auto const target = jump_targets.current();
                                        if (!image.red_jumpable(jumpee, target))
                                            continue;
                                        Color c = image.get(target);
                                        // If blue can hit RED on base we assume
                                        // that RED soldier is able to leave on
                                        // the next move making room for BLUE
                                        if (c == RED) {
                                            if (target.base_red()) {
                                                // logger << "Reach:\n" << image << flush;
                                                goto ACCEPTABLE;
                                            }
                                            continue;
                                        }
                                        // Getting here means target is EMPTY
                                        image.set(target, COLORS);
                                        reachable[nr_reachable++] = target;
                                    }
                                }
                                // Check if any of the positions reachable for
                                // BLUE can use a RED soldier sliding off base
                                // to jump into the resulting hole
                                for (uint i = 0; i < nr_reachable; ++i) {
                                    auto pos = reachable[i];
                                    uint n = pos.nr_slide_jumps_red();
                                    for (uint j=0; j<n; ++j) {
                                        auto slider = pos.slide_jumps_red()[j];
                                        if (image.get(slider) == RED) {
                                            // logger << "Slide:\n" << image << flush;
                                            goto ACCEPTABLE;
                                        }
                                    }
                                }
                                // logger << "Fail:\n" << image << flush;
                                for (uint i = blues; i < nr_reachable; ++i)
                                    image.set(reachable[i], EMPTY);
                                image.set(val, EMPTY);
                                image.set(soldier, BLUE);
                                if (VERBOSE) {
                                    logger << "   Move " << soldier << " to " << val << "\n";
                                    logger << "   Prune deep red base is full and red cannot move away fast enough\n";
                                    logger.flush();
                                }
                                continue;
                              ACCEPTABLE:
                                for (uint i = blues; i < nr_reachable; ++i)
                                    image.set(reachable[i], EMPTY);

                              SLIDABLE:;
                            }

                            bool escape = true;
                            if (off <= tables.min_surround()) {
                                array<Coord, MAX_ARMY> reachable;
                                array<Color, MAX_ARMY> old_color;
                                int j = tables.nr_deep_red();
                                while (--j >= 0) {
                                    reachable[0] = tables.deep_red_base(j);
                                    Color color = image.get(reachable[0]);
                                    if (color == BLUE) continue;
                                    old_color[0] = color;
                                    image.set(reachable[0], COLORS);
                                    uint nr_reachable = 1;
                                    for (uint i=0; i<nr_reachable; ++i) {
                                        auto slide_targets = reachable[i].slide_targets();
                                        auto jump_targets = reachable[i].jump_targets();
                                        for (uint r = 0; r < RULES; ++r, slide_targets.next(), jump_targets.next()) {
                                            auto const slide_target = slide_targets.current();
                                            color = image.get(slide_target);
                                            if (color != BLUE) {
                                                if (!slide_target.deep_red()) goto ESCAPE;
                                                if (color != COLORS) {
                                                    image.set(slide_target, COLORS);
                                                    old_color[nr_reachable] = color;
                                                    reachable[nr_reachable] = slide_target;
                                                    ++nr_reachable;
                                                }
                                            }
                                            auto const jump_target = jump_targets.current();
                                            color = image.get(jump_target);
                                            if (color != BLUE) {
                                                if (!jump_target.deep_red()) goto ESCAPE;
                                                if (color != COLORS) {
                                                    image.set(jump_target, COLORS);
                                                    old_color[nr_reachable] = color;
                                                    reachable[nr_reachable] = jump_target;
                                                    ++nr_reachable;
                                                }
                                            }
                                        }
                                    }
                                    for (uint i=0; i<nr_reachable; ++i)
                                        image.set(reachable[i], old_color[i]);
                                    escape = false;
                                    break;

                                  ESCAPE:;
                                    for (uint i=0; i<nr_reachable; ++i)
                                        image.set(reachable[i], old_color[i]);
                                }
                            }

                            image.set(val, EMPTY);
                            image.set(soldier, BLUE);

                            if (!escape) {
                                if (VERBOSE) {
                                    logger << "   Move " << soldier << " to " << val << "\n";
                                    logger << "   Prune deep red base is surrounded\n";
                                    logger.flush();
                                }
                                continue;
                            }
                        }
#else  // BLUE_TO_MOVE
                        if (blue_base_only) {
                            uint r_top = red_top;

                            if (slides) {
                                for (uint i = r_top+1; i < reachable.size(); ++i) {
                                    auto targets = reachable[i].slide_targets();
                                    for (uint r = 0; r < RULES; ++r, targets.next()) {
                                        auto const target = targets.current();
                                        if (target.base_red()) continue;
                                        if (image.get(target) == BLUE)
                                            goto SLIDABLE;
                                    }
                                }
                            }

                            image.set(soldier, EMPTY);
                            for (uint i = r_top+1; i < reachable.size(); ++i)
                                image.set(reachable[i], COLORS);
                            // Setting pos val must be able to override COLORS
                            // since val can be shallow red leading to a false
                            // EMPTY
                            image.set(val, RED);

                            // logger << "Start mass backtrack:\n" << image;
                            for (uint i = reachable.size()-1; i > r_top; --i) {
                                auto jumpees      = reachable[i].jumpees();
                                auto jump_targets = reachable[i].jump_targets();
                                for (uint r = 0; r < RULES; ++r, jumpees.next(), jump_targets.next()) {
                                    auto const jumpee = jumpees.current();
                                    auto const target = jump_targets.current();
                                    if (!image.blue_jumpable(jumpee, target))
                                        continue;
                                    Color c = image.get(target);
                                    if (c == BLUE) {
                                        if (!target.base_red()) goto ACCEPTABLE;
                                        continue;
                                    }
                                    image.set(target, COLORS);
                                    reachable[r_top--] = target;
                                }
                            }
                            // logger << "Failed mass backtrack:\n" << image;
                            for (uint i = reachable.size()-1; i > r_top; --i)
                                image.set(reachable[i], EMPTY);
                            image.set(val, EMPTY);
                            image.set(soldier, RED);
                            if (VERBOSE) {
                                logger << "   Move " << soldier << " to " << val << "\n";
                                logger << "   Prune blue cannot jump to target\n";
                                logger.flush();
                            }
                            continue;
                          ACCEPTABLE:
                            // logger << "Succeeded mass backtrack:\n" << image;
                            for (uint i = reachable.size()-1; i > r_top; --i)
                                image.set(reachable[i], EMPTY);
                            image.set(val, EMPTY);
                            image.set(soldier, RED);

                          SLIDABLE:
                            r_top = red_top + val.shallow_red();
                            uint r_empty = reachable.size()-1 - r_top;
                            if (r_empty <= 1) {
                                if (r_empty < 1) {
                                    // Shallow full. Red moved into base
                                    // (Since there was an EMPTY that got
                                    //  succesfully backtracked)
                                    if (VERBOSE) {
                                        logger << "   Move " << soldier << " to " << val << "\n";
                                        logger << "   Prune red base blocked\n";
                                        logger.flush();
                                    }
                                    continue;
                                }
                                if (deep_red_empty + soldier.deep_red() - val.deep_red() == 0) {
                                    if (VERBOSE) {
                                        logger << "   Move " << soldier << " to " << val << "\n";
                                        logger << "   Prune deep out of time\n";
                                        logger.flush();
                                    }
                                    continue;
                                }
                            }
                        }
#endif // BLUE_TO_MOVE
                    }
#if BLUE_TO_MOVE
                  SOLUTION:
#endif // BLUE_TO_MOVE
                    armyE.store(val);
                    // logger << "   Final Set pos " << pos << armyE[pos] << "\n";
                    // logger << armyE << "----------------\n";
                    // logger.flush();
                    if (CHECK) armyE.check(__FILE__, __LINE__);
                    armyESymmetric.store(val.symmetric());
                    if (CHECK) armyESymmetric.check(__FILE__, __LINE__);
                    int result_symmetry = cmp(armyE, armyESymmetric);
                    auto moved_id = moved_armies.insert(result_symmetry >= 0 ? armyE : armyESymmetric, stats);
                    if (CHECK && UNLIKELY(moved_id == 0))
                        throw_logic("Army Insert returns 0", __FILE__, __LINE__);
#if BLUE_TO_MOVE
                    // The opponent is red and after this it is red's move
                    result_symmetry *= red_symmetry;
                    if (boards_to.insert(moved_id, red_id, result_symmetry, stats)) {
                        if (edge_c) stats.edge();
                        if (VERBOSE) {
                            // logger << "   symmetry=" << result_symmetry << "\n   armyE:\n" << armyE << "   armyESymmetric:\n" << armyESymmetric;
                            logger << "   Inserted Blue id " << moved_id << "\n" << image.str(soldier, val, BLUE) << flush;
                        }
                    }
#else  // BLUE_TO_MOVE
                    // The opponent is blue and after this it is blue's move
                    result_symmetry *= blue_symmetry;
                    if (subset_to.insert(moved_id, result_symmetry, stats)) {
                        if (VERBOSE) {
                            // logger << "   symmetry=" << result_symmetry << "\n   armyE:\n" << armyE << "   armyESymmetric:\n" << armyESymmetric;
                            logger << "   Inserted Red id " << moved_id << "\n" << image.str(soldier, val, RED) << flush;
                        }
                    }
#endif // BLUE_TO_MOVE
                    else if (VERBOSE) {
                        logger << "   Move " << soldier << " to " << val << "\n";
                        logger << "   Duplicate\n";
                        logger.flush();
                    }
                }
            }
            if (CHECK) image.check(__FILE__, __LINE__);
            END_GUARD;
        }
#if !BLUE_TO_MOVE
        if (edge_count_from) stats.edge(subset_to.size());
        if (STATISTICS) stats.subset_size(subset_to.size());
        boards_to.insert(blue_id, subset_to);
#endif // !BLUE_TO_MOVE
        END_GUARD;
        END_GUARD;
    }

    // logger << "Stopped (Set " << available_moves << ")\n" << flush;
    return stats;
}

# define ALL_NAME CAT(make_all_,CAT(COLOR_TO_MOVE,CAT(_moves,CAT(_BACKTRACK,_SLOW))))

StatisticsE ALL_NAME(
#if BLUE_TO_MOVE
                     BoardSetRed & boards_from,
                     BoardSetBlue& boards_to,
#else // BLUE_TO_MOVE
                     BoardSetBlue& boards_from,
                     BoardSetRed & boards_to,
#endif // BLUE_TO_MOVE
                     ArmySet const& moving_armies,
                     ArmySet const& opponent_armies,
                     ArmySet& moved_armies,
#if BACKTRACK && !BLUE_TO_MOVE
                     int solution_moves,
                     BoardTable<uint8_t> const& red_backtrack,
                     BoardTable<uint8_t> const& red_backtrack_symmetric,
#endif // BACKTRACK
                     int nr_moves) {
    StatisticsE stats{nr_moves, opponent_armies.size()};
    stats.start();
    stats.armyset_untry(moved_armies.size());
    stats.boardset_untry(boards_to.size());

    boards_from.pre_read();
    uint n = boards_to.pre_write();
    vector<future<Statistics>> results;

    for (uint i=1; i < n; ++i)
        results.emplace_back
            (async
             (launch::async, NAME,
              i, ref(boards_from), ref(boards_to),
              ref(moving_armies), ref(opponent_armies), ref(moved_armies),
#if BACKTRACK && !BLUE_TO_MOVE
              ref(red_backtrack), ref(red_backtrack_symmetric), solution_moves,
#endif // BACKTRACK && !BLUE_TO_MOVE
              nr_moves));
    static_cast<Statistics&>(stats) = NAME
    (0, boards_from, boards_to,
     moving_armies, opponent_armies, moved_armies,
#if BACKTRACK && !BLUE_TO_MOVE
     red_backtrack, red_backtrack_symmetric, solution_moves,
#endif // BACKTRACK && !BLUE_TO_MOVE
     nr_moves);

    for (auto& result: results) stats += result.get();

    if (MEMORY_REPORT)
        memory_report(cout,
                      moving_armies, opponent_armies, moved_armies,
                      boards_from, boards_to);

    boards_to.post_write();
    boards_from.post_read();

    stats.overflow(moved_armies.overflow_max());
    stats.armyset_size(moved_armies.size());
    stats.boardset_size(boards_to.size());
#if BLUE_TO_MOVE
    stats.largest_subset_size(boards_to);
#endif // BLUE_TO_MOVE
    stats.stop();

    return stats;
}

# undef ALL_NAME

#undef COLOR_TO_MOVE
#undef NAME
#undef _BACKTRACK
#undef _SLOW
#undef VERBOSE
