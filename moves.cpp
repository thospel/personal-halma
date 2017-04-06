// uint make_moves_backtrack(Army const& army, Army const& army_symmetric,
//                          Army const& opponent_army, ArmyId opponent_id, int opponent_symmetry,
//                          BoardSet& board_set, ArmySet& army_set, int available_moves, bool demand, BoardTable<uint8_t> const& red_demand) {
{
    uint8_t blue_to_move = available_moves & 1;
    Army const& blue = blue_to_move ? army : opponent_army;
    Army const& red  = blue_to_move ? opponent_army : army;
    Image image{blue, red};
    if (VERBOSE) cout << "From: " << available_moves << "\n" << image;

    Nbits Ndistance_army, Ndistance_red;
    Ndistance_army = Ndistance_red = NLEFT >> tables.infinity();
    int off_base_from = 0;
    TypeCount type_count_from = tables.type_count();
    int edge_count_from = 0;
    for (auto const& b: blue) {
        --type_count_from[tables.type(b)];
        if (tables.base_red(b)) continue;
        ++off_base_from;
        edge_count_from += tables.edge_red(b);
        Ndistance_red |= tables.Ndistance_base_red(b);
        for (auto const& r: red)
            Ndistance_army |= tables.Ndistance(r, b);
    }
    int slides = 0;
    for (auto tc: type_count_from)
        slides += max(tc, 0);
    int distance_army = __builtin_clz(Ndistance_army);
    int distance_red  = __builtin_clz(Ndistance_red);

    if (VERBOSE) {
        cout << "Slides >= " << slides << ", red edge count " << edge_count_from << "\n";
        cout << "Distance army=" << distance_army << "\n";
        cout << "Distance red =" << distance_red  << "\n";
        cout << "Off base=" << off_base_from << "\n";
    }
    int pre_moves = min((distance_army + blue_to_move) / 2, distance_red);
    int blue_moves = pre_moves + max(slides-pre_moves-edge_count_from, 0) + off_base_from;
    int needed_moves = 2*blue_moves - blue_to_move;
    if (VERBOSE)
        cout << "Needed moves=" << static_cast<int>(needed_moves) << "\n";
    if (needed_moves > available_moves) {
        if (VERBOSE)
            cout << "Late prune " << needed_moves << " > " << available_moves << "\n";
        return 1;
    }
    --available_moves;

    ArmyPos armyE, armyESymmetric;
    auto off_base   = off_base_from;
    auto type_count = type_count_from;
    auto edge_count = edge_count_from;
    ArmyMapper mapper{army_symmetric};

    int red_demand_count_from = 2*ARMY;
    int red_demand_count_symmetric_from =  2*ARMY;
    if (demand && !blue_to_move) {
        for (auto const& pos: army)
            red_demand_count_from -= red_demand[pos];
        for (auto const& pos: army_symmetric)
            red_demand_count_symmetric_from -= red_demand[pos];
    }
    int red_demand_count = 0;
    int red_demand_count_symmetric = 0;
    for (int a=0; a<ARMY; ++a) {
        armyE.copy(army, a);
        auto const soldier = army[a];
        image.set(soldier, EMPTY);
        armyESymmetric.copy(army_symmetric, mapper.map(soldier));
        if (blue_to_move) {
            off_base = off_base_from;
            off_base += tables.base_red(soldier);
            type_count = type_count_from;
            ++type_count[tables.type(soldier)];
            edge_count = edge_count_from;
            edge_count -= tables.edge_red(soldier);
        } else if (demand) {
            red_demand_count = red_demand_count_from + red_demand[soldier];
            red_demand_count_symmetric = red_demand_count_symmetric_from + red_demand[soldier.symmetric()];
        }

        // Jumps
        array<Coord, ARMY*2*MOVES+(1+MOVES)> reachable;
        reachable[0] = soldier;
        int nr_reachable = 1;
        if (!CLOSED_LOOP) image.set(soldier, COLORS);
        for (int i=0; i < nr_reachable; ++i) {
            for (auto move: tables.moves()) {
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

        // Step moves
        for (auto move: tables.moves()) {
            Coord target{soldier, move};
            if (image.get(target) != EMPTY) continue;
            reachable[nr_reachable++] = target;
        }

        for (int i=1; i < nr_reachable; ++i) {
            // armyZ[a] = CoordZ{reachable[i]};
            if (false) {
                image.set(reachable[i], RED - blue_to_move);
                cout << image;
                image.set(reachable[i], EMPTY);
            }
            auto val = reachable[i];

            Nbits Ndistance_a = Ndistance_army;
            if (blue_to_move) {
                Nbits Ndistance_r = Ndistance_red;
                auto off    = off_base;
                auto type_c = type_count;
                auto edge_c = edge_count;

                if (tables.base_red(val)) {
                    --off;
                    if (off == 0) {
                        if (!demand &&
                            board_set.solve(opponent_id, opponent_army)) {
                            image.set(reachable[i], RED - blue_to_move);
                            cout << "==================================\n";
                            cout << image << "Solution!" << endl;
                            image.set(reachable[i], EMPTY);
                        }
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
                --type_c[tables.type(val)];
                int slides = 0;
                for (auto tc: type_c)
                    slides += max(tc, 0);
                int blue_moves = pre_moves + max(slides-pre_moves-edge_c, 0) + off;
                needed_moves = 2*blue_moves;
            } else {
                if (demand &&
                    red_demand_count - red_demand[val] > available_moves &&
                    red_demand_count_symmetric - red_demand[val.symmetric()] > available_moves) continue;
                // We won't notice an increase in army distance, but these
                // are rare and will be discovered in the late prune
                for (auto const& b: blue) {
                    if (tables.base_red(b)) continue;
                    Ndistance_a |= tables.Ndistance(val, b);
                }
                int distance_army = __builtin_clz(Ndistance_a);
                int pre_moves = min((distance_army + 1) / 2, distance_red);
                int blue_moves = pre_moves + max(slides-pre_moves-edge_count_from, 0) + off_base_from;
                needed_moves = 2*blue_moves - 1;
            }
            if (needed_moves > available_moves) {
                if (VERBOSE) {
                    cout << "Move " << soldier << " to " << val << "\n";
                    cout << "Prune " << needed_moves << " > " << available_moves << "\n";
                }
                continue;
            }

          SOLUTION:
            armyE.store(val);
            // cout << "Final Set pos " << pos << armyE[pos] << "\n";
            // cout << armyE << "----------------\n";
            if (CHECK) armyE.check(__LINE__);
            armyESymmetric.store(val.symmetric());
            if (CHECK) armyESymmetric.check(__LINE__);
            int symmetry = cmp(armyE, armyESymmetric);
            auto moved_index = army_set.insert(symmetry >= 0 ? armyE : armyESymmetric);
            if (CHECK && moved_index == 0)
                throw(logic_error("Army Insert returns 0"));
            symmetry *= opponent_symmetry;
            if (board_set.insert(opponent_id, moved_index, symmetry)) {
                if (VERBOSE) {
                    if (blue_to_move)
                        cout << Image{armyE, opponent_army};
                    else
                        cout << Image{opponent_army, armyE};
                }
            }
        }

        image.set(soldier, RED - blue_to_move);
    }
    return 0;
}
