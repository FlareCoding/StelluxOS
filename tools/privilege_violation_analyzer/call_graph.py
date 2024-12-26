from collections import defaultdict
from symbol_analyzer import SymbolAnalyzer
from colorama import Fore, Style, init as colorama_init

# Initialize colorama
colorama_init(autoreset=True)


class CallGraphBuilder:
    def __init__(self, symbols):
        """
        Initialize with a list of symbol dictionaries, each containing:
        - 'name': function name
        - 'address': function start address
        - 'instructions': list of disassembled instructions
        - 'privileged': boolean indicating if the function is privileged
        """
        self.symbols = symbols
        self.call_graph = defaultdict(list)  # Maps caller address to list of callee edges
        self.addr_to_symbol = {sym['address']: sym for sym in symbols}
        self.called_addresses = set()
        self.call_paths = None
        self.privilege_violations = []
        self.privilege_warnings = []

    def _create_call_edge(self, caller, callee, elevated_call_privilege=False):
        """
        Create a structured call edge as a dictionary.
        """
        return {
            "caller": caller,
            "callee": callee,
            "elevated_call_privilege": elevated_call_privilege,
        }

    def build_graph(self):
        """
        Build the call graph based on disassembled instructions.
        """
        for sym in self.symbols:
            caller_addr = sym['address']
            for instruction in sym['instructions']:
                mnemonic = instruction["mnemonic"]
                op_str = instruction["op_str"]
                elevated_call_privilege = instruction["elevated_call_privilege"]

                if mnemonic == "call" and op_str.startswith("0x"):
                    try:
                        callee_addr = int(op_str.split()[0], 16)  # Extract address from "0xADDR <symbol>"
                        edge = self._create_call_edge(caller=caller_addr, callee=callee_addr,
                                                      elevated_call_privilege=elevated_call_privilege)
                        self.call_graph[caller_addr].append(edge)
                        self.called_addresses.add(callee_addr)
                    except ValueError:
                        pass

        self.call_paths = self.generate_call_paths()
        self.analyze_privilege_violations()

    def find_root_functions(self):
        """
        Identify root functions (not called by any other function).
        """
        root_functions = []
        for sym in self.symbols:
            if sym['address'] not in self.called_addresses:
                root_functions.append(sym)
        return root_functions

    def generate_call_paths(self):
        """
        Generate all call paths starting from root functions.
        Returns a list of paths, each represented as a list of tuples (function name, privilege, address).
        """
        root_functions = self.find_root_functions()
        paths = []

        def dfs(current_path, current_addr, visited, is_privileged):
            if current_addr in visited:
                return  # Prevent cycles
            if current_addr not in self.addr_to_symbol:
                return  # Skip unknown addresses

            visited.add(current_addr)
            symbol = self.addr_to_symbol[current_addr]
            current_privilege = is_privileged or symbol['privileged']
            current_path.append((symbol['name'], current_privilege, current_addr))

            if current_addr not in self.call_graph:
                paths.append(list(current_path))  # Leaf node, save the path
            else:
                for edge in self.call_graph[current_addr]:
                    dfs(current_path, edge["callee"], visited, current_privilege)

            current_path.pop()
            visited.remove(current_addr)

        for root in root_functions:
            dfs([], root['address'], set(), root['privileged'])

        return paths

    def analyze_privilege_violations(self):
        """
        Analyze the call paths and detect privilege violations and warnings.
        A privilege violation occurs when:
        - An unprivileged function calls a privileged function.

        A privilege warning occurs when:
        - An unprivileged function that inherited privilege in the current call chain calls a privileged function.

        Stores violations in `self.privilege_violations` and warnings in `self.privilege_warnings`.
        """
        self.privilege_violations = []  # Clear previous violations
        self.privilege_warnings = []    # Clear previous warnings
        seen_violations = set()         # Track unique violations
        seen_warnings = set()           # Track unique warnings

        def check_for_violations_and_warnings(path):
            for i in range(len(path) - 1):
                caller_name, caller_privileged, caller_addr = path[i]
                callee_name, callee_privileged, callee_addr = path[i + 1]

                # Find the corresponding call edge for elevated privilege check
                edge_key = (caller_addr, callee_addr)
                call_edges = self.call_graph.get(caller_addr, [])
                elevated_call_privilege = any(
                    edge["callee"] == callee_addr and edge.get("elevated_call_privilege", False)
                    for edge in call_edges
                )

                # Detect privilege violation
                if not caller_privileged and callee_privileged and not elevated_call_privilege:
                    violation_key = (
                        caller_name, caller_addr, callee_name, callee_addr,
                        tuple((name, privileged) for name, privileged, _ in path[:i + 2])  # Path snapshot
                    )
                    if violation_key not in seen_violations:
                        seen_violations.add(violation_key)
                        violation = {
                            "caller": {"name": caller_name, "address": caller_addr},
                            "callee": {"name": callee_name, "address": callee_addr},
                            "path": path[:i + 2],  # The path leading to the violation
                        }
                        self.privilege_violations.append(violation)

                # Detect privilege warning
                if caller_privileged and not elevated_call_privilege and not caller_name.startswith("dynpriv::"):
                    warning_key = (
                        caller_name, caller_addr, callee_name, callee_addr,
                        tuple((name, privileged) for name, privileged, _ in path[:i + 2])  # Path snapshot
                    )
                    if callee_privileged and warning_key not in seen_warnings:
                        seen_warnings.add(warning_key)
                        warning = {
                            "caller": {"name": caller_name, "address": caller_addr},
                            "callee": {"name": callee_name, "address": callee_addr},
                            "path": path[:i + 2],  # The path leading to the warning
                        }
                        self.privilege_warnings.append(warning)

        for path in self.call_paths:
            check_for_violations_and_warnings(path)

    def print_call_graph_tree(self, sym=None):
        """
        Print the call graph as an ASCII tree with privilege state, marking elevated calls and avoiding duplicates.
        """
        def print_tree(node_addr, prefix="", visited_nodes=None, visited_edges=None, inherited_privilege=False):
            if visited_nodes is None:
                visited_nodes = set()
            if visited_edges is None:
                visited_edges = set()

            if node_addr in visited_nodes:
                return  # Prevent cycles in the graph

            symbol = self.addr_to_symbol.get(node_addr)
            if not symbol:
                return  # Skip unknown addresses

            visited_nodes.add(node_addr)
            name = SymbolAnalyzer.simplify_symbol_name(symbol['name'])
            privilege_state = "privileged" if inherited_privilege or symbol['privileged'] else "unprivileged"
            print(f"{prefix}└── {name} ({privilege_state})")

            children = self.call_graph.get(node_addr, [])
            for i, edge in enumerate(children):
                edge_key = (edge["caller"], edge["callee"])
                if edge_key in visited_edges:
                    continue  # Skip duplicate edges

                visited_edges.add(edge_key)
                child_addr = edge["callee"]
                child_inherited_privilege = inherited_privilege or symbol['privileged']
                elevated_marker = f" {Fore.YELLOW}[Elevated]" if edge["elevated_call_privilege"] else ""

                is_last = (i == len(children) - 1)
                connector = "└──" if is_last else "├──"
                new_prefix = prefix + ("    " if is_last else "│   ")

                child_symbol = self.addr_to_symbol.get(child_addr, {})
                child_name = SymbolAnalyzer.simplify_symbol_name(child_symbol.get("name", "unknown"))
                child_privilege_state = "privileged" if child_inherited_privilege else "unprivileged"

                print(f"{prefix}{connector} {child_name} ({child_privilege_state}){elevated_marker}")
                print_tree(child_addr, new_prefix, visited_nodes, visited_edges, child_inherited_privilege)

        root_functions = self.find_root_functions()
        for root in root_functions:
            if sym is not None and sym not in root['name']:
                continue
            print(f"{root['name']}:")
            print_tree(root['address'])


    def print_privilege_violations(self, sym=None):
        """
        Print detected privilege violations in a visually enhanced, aligned, and indented format.
        """
        if not self.privilege_violations:
            print(f"{Fore.GREEN}[No privilege violations detected]")
            return

        def print_violation(violation):
            path = violation["path"]
            print(f"\n{Fore.LIGHTBLACK_EX}Call Stack:")
            for i, (name, privileged, addr) in enumerate(path):
                privilege_state = f"{Fore.RED}(privileged)  " if privileged else f"{Fore.WHITE}(unprivileged)"
                color = Fore.WHITE  # Default to white for unprivileged functions

                if not privileged and i < len(path) - 1:
                    # Check if this unprivileged function leads to the violation
                    _, next_privileged, _ = path[i + 1]
                    if next_privileged:
                        color = Fore.YELLOW  # Highlight the violation-causing unprivileged function
                        privilege_state = f"{Fore.RED}(privileged)  " if privileged else f"{Fore.YELLOW}(unprivileged)"
                elif privileged:
                    color = Fore.RED  # Highlight privileged functions

                prefix = f"{Fore.LIGHTBLACK_EX}└── " if i == len(path) - 1 else f"{Fore.LIGHTBLACK_EX}│   "
                addr_str = f"0x{addr:x}"  # Lowercase hexadecimal
                print(f"  {prefix}{color}{SymbolAnalyzer.simplify_symbol_name(name):<60} {privilege_state} {Fore.LIGHTBLACK_EX}[{addr_str}]")

        for idx, violation in enumerate(self.privilege_violations):
            if sym is not None and sym not in violation["path"][0][0]:
                continue
            caller = violation["caller"]
            callee = violation["callee"]

            name_alignment = max(len(caller['name']), len(callee['name']))

            print(f"\n{Fore.LIGHTBLACK_EX}{'=' * 40}")
            print(f"{Fore.LIGHTCYAN_EX}[ {Fore.LIGHTRED_EX}⚠ Violation {idx + 1} ⚠ {Fore.LIGHTCYAN_EX}]".center(40))
            print(f"{Fore.LIGHTBLACK_EX}{'=' * 40}")
            print(f"{Fore.LIGHTWHITE_EX}  Caller: {Fore.CYAN}{caller['name']:<{name_alignment}} {Fore.LIGHTBLACK_EX}[0x{caller['address']:x}]")
            print(f"{Fore.LIGHTWHITE_EX}  Callee: {Fore.LIGHTRED_EX}{callee['name']:<{name_alignment}} {Fore.LIGHTBLACK_EX}[0x{callee['address']:x}]")
            print_violation(violation)

    def print_privilege_warnings(self, sym=None):
        """
        Print detected privilege warnings in a visually enhanced, aligned, and indented format.
        """
        if not self.privilege_warnings:
            print(f"{Fore.GREEN}[No privilege warnings detected]")
            return

        def print_warning(warning):
            path = warning["path"]
            print(f"\n{Fore.LIGHTBLACK_EX}Call Stack:")
            for i, (name, privileged, addr) in enumerate(path):
                privilege_state = f"{Fore.RED}(privileged)" if privileged else f"{Fore.WHITE}(unprivileged)"
                prefix = f"{Fore.LIGHTBLACK_EX}└── " if i == len(path) - 1 else f"{Fore.LIGHTBLACK_EX}│   "
                addr_str = f"0x{addr:x}"  # Lowercase hexadecimal
                print(f"  {prefix}{name:<60} {privilege_state} {Fore.LIGHTBLACK_EX}[{addr_str}]")

        for idx, warning in enumerate(self.privilege_warnings):
            if sym is not None and sym not in warning["path"][0][0]:
                continue
            caller = warning["caller"]
            callee = warning["callee"]

            print(f"\n{Fore.LIGHTBLACK_EX}{'=' * 40}")
            print(f"{Fore.LIGHTCYAN_EX}[ {Fore.LIGHTYELLOW_EX}⚠ Warning {idx + 1} ⚠ {Fore.LIGHTCYAN_EX}]".center(40))
            print(f"{Fore.LIGHTBLACK_EX}{'=' * 40}")
            print(f"{Fore.LIGHTWHITE_EX}  Caller: {Fore.CYAN}{caller['name']} {Fore.LIGHTBLACK_EX}[0x{caller['address']:x}]")
            print(f"{Fore.LIGHTWHITE_EX}  Callee: {Fore.LIGHTRED_EX}{callee['name']} {Fore.LIGHTBLACK_EX}[0x{callee['address']:x}]")
            print_warning(warning)
