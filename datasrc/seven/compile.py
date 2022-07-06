import sys
from .datatypes import EmitDefinition, EmitTypeDeclaration
from . import content

def create_enum_table(names, num):
	lines = []
	lines += ["enum", "{"]
	lines += ["\t%s=0,"%names[0]]
	for name in names[1:]:
		lines += ["\t%s,"%name]
	lines += ["\t%s" % num, "};"]
	return lines

def create_flags_table(names):
	lines = []
	lines += ["enum", "{"]
	i = 0
	for name in names:
		lines += ["\t%s = 1<<%d," % (name,i)]
		i += 1
	lines += ["};"]
	return lines

def EmitEnum(names, num):
	print("enum")
	print("{")
	print("\t%s=0," % names[0])
	for name in names[1:]:
		print("\t%s," % name)
	print("\t%s" % num)
	print("};")

def EmitFlags(names):
	print("enum")
	print("{")
	i = 0
	for name in names:
		print("\t%s = 1<<%d," % (name,i))
		i += 1
	print("};")

def main():
	gen_client_content_header = "client_content_header" in sys.argv
	gen_client_content_source = "client_content_source" in sys.argv

	if gen_client_content_header:
		print("#ifndef CLIENT_CONTENT7_HEADER")
		print("#define CLIENT_CONTENT7_HEADER")

	if gen_client_content_header:
		# print some includes
		print('#include <engine/graphics.h>')
		print('#include <engine/sound.h>')
		print("namespace client_data7 {")

		# emit the type declarations
		contentlines = open("datasrc/content.py", "rb").readlines()
		order = []
		for line in contentlines:
			line = line.strip()
			if line[:6] == "class ".encode() and "(Struct)".encode() in line:
				order += [line.split()[1].split("(".encode())[0].decode("ascii")]
		for name in order:
			EmitTypeDeclaration(content.__dict__[name])

		# the container pointer
		print('extern CDataContainer *g_pData;')

		# enums
		EmitEnum(["IMAGE_%s"%i.name.value.upper() for i in content.container.images.items], "NUM_IMAGES")
		EmitEnum(["ANIM_%s"%i.name.value.upper() for i in content.container.animations.items], "NUM_ANIMS")
		EmitEnum(["SPRITE_%s"%i.name.value.upper() for i in content.container.sprites.items], "NUM_SPRITES")

	if gen_client_content_source:
		if gen_client_content_source:
			print('#include "client_data7.h"')
		print("namespace client_data7 {")
		EmitDefinition(content.container, "datacontainer")
		print('CDataContainer *g_pData = &datacontainer;')
		print("}")

	if gen_client_content_header:
		print("}")
		print("#endif")

if __name__ == '__main__':
	main()
