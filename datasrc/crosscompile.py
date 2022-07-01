import sys
import network

def get_msgs():
	return ["NETMSG_INVALID"] + [m.enum_name for m in network.Messages]

def get_objs():
	return ["NETOBJ_INVALID"] + [m.enum_name for m in network.Objects if m.ex is None]

def generate_map(a, b):
	result = []
	for m in a:
		try:
			result += [b.index(m)]
		except ValueError:
			result += [-1]

	return result

def output_map_header(name, m):
	print("extern const int gs_{}[{}];".format(name, len(m)))
	print("inline int {0}(int a) {{ if(a < 0 || a >= {1}) return -1; return gs_{0}[a]; }}".format(name, len(m)))

def output_map_source(name, m):
	print("const int gs_{}[{}] = {{".format(name, len(m)))
	print(*m, sep=',')
	print("};")

def main():
	map_header = "map_header" in sys.argv
	map_source = "map_source" in sys.argv
	guard = "GAME_GENERATED_PROTOCOLGLUE"
	if map_header:
		print("#ifndef " + guard)
		print("#define " + guard)
	elif map_source:
		print("#include \"protocolglue.h\"")

if __name__ == "__main__":
	main()
