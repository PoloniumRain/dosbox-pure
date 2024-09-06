/*
 *  Copyright (C) 2020-2023 Bernhard Schelling
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

static const char* DBP_MachineNames[] = { "SVGA (Super Video Graphics Array)", "VGA (Video Graphics Array)", "EGA (Enhanced Graphics Adapter", "CGA (Color Graphics Adapter)", "Tandy (Tandy Graphics Adapter", "Hercules (Hercules Graphics Card)", "PCjr" };

struct DBP_Run
{
	static void RunBatchFile(BatchFile* bf)
	{
		DBP_ASSERT(!dbp_game_running);
		const bool inAutoexec = (first_shell->bf && first_shell->bf->filename[0] == 'Z');
		while (first_shell->bf) delete first_shell->bf;
		bf->prev = NULL; // was just deleted
		bf->echo = true; // always want this back after returning
		first_shell->bf = bf;
		first_shell->echo = false;
		if (!inAutoexec)
		{
			// Sending this key sequence makes sure DOS_Shell::Run will run our batch file immediately
			// It also clears anything typed already on the command line or finishes DOS_Shell::CMD_PAUSE or DOS_Shell::CMD_CHOICE
			KEYBOARD_AddKey(KBD_esc, true);
			KEYBOARD_AddKey(KBD_esc, false);
			KEYBOARD_AddKey(KBD_enter, true);
			KEYBOARD_AddKey(KBD_enter, false);
		}
		dbp_lastmenuticks = DBP_GetTicks();
	}

	static void ConsoleClearScreen()
	{
		DBP_ASSERT(!dbp_game_running);
		reg_ax = 0x0003;
		CALLBACK_RunRealInt(0x10);
	}

	struct BatchFileExec : BatchFile
	{
		BatchFileExec(const std::string& _exe) : BatchFile(first_shell,"Z:\\AUTOEXEC.BAT","","") { filename = _exe; if (!_exe.length()) location = 1; }
		virtual bool ReadLine(char * line)
		{
			*(line++) = '@';
			switch (location++)
			{
				case 0:
				{
					ConsoleClearScreen();
					char *fn = (char*)filename.c_str(), *r = fn + ((fn[0] && fn[1] == ':') ? 2 : 0), *p = r + (*r == '\\' ? 1 : 0), *param = strchr(p, ' '), *sl;
					if (param) { *param = '\0'; sl = strrchr(p, '\\'); *param = ' '; } else { sl = strrchr(p, '\\'); };
					const Bit8u drive = ((((fn[0] >= 'A' && fn[0] <= 'Z') || (fn[0] >= 'a' && fn[0] <= 'z')) && fn[1] == ':') ? (fn[0] & 0x5F) : 'C') - 'A';
					if (Drives[drive])
					{
						DOS_SetDefaultDrive(drive);
						if (sl)
						{
							memcpy(Drives[drive]->curdir,p, sl - p);
							Drives[drive]->curdir[sl - p] = '\0';
						}
						else Drives[drive]->curdir[0] = '\0';
					}
					else { sl = NULL; p = fn; } // try call full string which will likely show an error to tell the user auto start is wrong

					const char* f = (sl  ? sl + 1 : p), *fext = strchr(f, '.');
					bool isbat = (fext && !strcasecmp(fext, ".bat"));
					int call_cmd_len = (isbat ? 5 : 0), flen = (int)strlen(f);
					memcpy(line, "call ", call_cmd_len);
					memcpy(line+call_cmd_len, f, flen);
					memcpy(line+call_cmd_len+flen, "\n", 2);
					break;
				}
				case 1:
					memcpy(line, "Z:PUREMENU", 10);
					memcpy(line+10, " -FINISH\n", 10);
					delete this;
					break;
			}
			return true;
		}
	};

	struct BatchFileBoot : BatchFile
	{
		BatchFileBoot(char drive) : BatchFile(first_shell,"Z:\\AUTOEXEC.BAT","","") { file_handle = drive; }

		virtual bool ReadLine(char * line)
		{
			if (location++)
			{
				// This function does not do `delete this;` instead it calls DBP_OnBIOSReboot to eventually do that
				memcpy(line, "@PAUSE\n", 8);
				if (location > 2) { startup.mode = RUN_NONE; DBP_OnBIOSReboot(); }
				return true;
			}
			ConsoleClearScreen();
			memcpy(line, "@Z:BOOT -l  \n", 14);
			line[11] = (char)file_handle; // drive letter
			if (machine == MCH_PCJR && file_handle == 'A' && !dbp_images.empty())
			{
				// The path to the image needs to be passed to boot for pcjr carts
				const std::string& imgpath = dbp_images[dbp_image_index].path;
				line[12] = ' ';
				memcpy(line+13, imgpath.c_str(), imgpath.size());
				memcpy(line+13+imgpath.size(), "\n", 2);
			}
			return true;
		}

		static bool HaveCDImage()
		{
			for (DBP_Image& i : dbp_images) if (DBP_Image_IsCD(i)) return true;
			return false;
		}

		static bool MountOSIMG(char drive, const char* path, const char* type, bool needwritable, bool complainnotfound)
		{
			FILE* raw_file_h = NULL;
			if (needwritable && (raw_file_h = fopen_wrap(path, "rb+")) != NULL) goto openok;
			if ((raw_file_h = fopen_wrap(path, "rb")) == NULL)
			{
				if (complainnotfound)
					retro_notify(0, RETRO_LOG_ERROR, "Unable to open %s file: %s%s", type, path, "");
				return false;
			}
			if (needwritable)
			{
				retro_notify(0, RETRO_LOG_ERROR, "Unable to open %s file: %s%s", type, path, " (file is read-only!)");
				fclose(raw_file_h);
				return false;
			}
			openok:
			DOS_File* df = new rawFile(raw_file_h, needwritable);
			df->AddRef();
			imageDiskList[drive-'A'] = new imageDisk(df, "", 0, true);
			imageDiskList[drive-'A']->Set_GeometryForHardDisk();
			return true;
		}
	};

	static void Exec(const std::string& _exe)
	{
		RunBatchFile(new BatchFileExec(_exe));
	}

	static void BootImage()
	{
		DBP_ASSERT(!dbp_images.empty()); // IT_BOOTIMG should only be available if this is true
		if (!dbp_images.empty())
		{
			DBP_Mount(); // make sure something is mounted

			// If hard disk image was mounted to D:, swap it to be the bootable C: drive
			std::swap(imageDiskList['D'-'A'], imageDiskList['C'-'A']);

			// If there is no mounted hard disk image but a D: drive, setup the CDROM IDE controller
			if (!imageDiskList['C'-'A'] && Drives['D'-'A'])
				IDE_SetupControllers(BatchFileBoot::HaveCDImage() ? 'D' : 0);

			// Install the NE2000 network card
			NET_SetupEthernet();
		}

		RunBatchFile(new BatchFileBoot(imageDiskList['A'-'A'] ? 'A' : 'C'));
	}

	static void BootOS(bool is_install, int osidx_or_size)
	{
		// Make sure we have at least 32 MB of RAM, if not set it to 64
		if ((MEM_TotalPages() / 256) < 32)
		{
			dbp_reboot_set64mem = true;
			DBP_OnBIOSReboot();
			return;
		}

		std::string path;
		if (!is_install)
		{
			path = DBP_GetSaveFile(SFT_SYSTEMDIR).append(dbp_osimages[osidx_or_size]);
		}
		else if (osidx_or_size)
		{
			const char* filename;
			path = DBP_GetSaveFile(SFT_NEWOSIMAGE, &filename);

			// Create a new empty hard disk image of the requested size
			memoryDrive* memDrv = new memoryDrive();
			DBP_SetDriveLabelFromContentPath(memDrv, path.c_str(), 'C', filename, path.c_str() + path.size() - 3);
			imageDisk* memDsk = new imageDisk(memDrv, (Bit32u)(osidx_or_size*8));
			Bit32u heads, cyl, sect, sectSize;
			memDsk->Get_Geometry(&heads, &cyl, &sect, &sectSize);
			FILE* f = fopen_wrap(path.c_str(), "wb");
			if (!f) { retro_notify(0, RETRO_LOG_ERROR, "Unable to open %s file: %s%s", "OS image", path.c_str(), " (create file failed)"); return; }
			for (Bit32u i = 0, total = heads * cyl * sect; i != total; i++) { Bit8u data[512]; memDsk->Read_AbsoluteSector(i, data); fwrite(data, 512, 1, f); }
			fclose(f);
			delete memDsk;
			delete memDrv;

			// If using system directory index cache, append the new OS image to that now
			if (dbp_system_cached)
				if (FILE* fc = fopen_wrap(DBP_GetSaveFile(SFT_SYSTEMDIR).append("DOSBoxPureMidiCache.txt").c_str(), "a"))
					{ fprintf(fc, "%s\n", filename); fclose(fc); }

			// Set last_info to this new image to support BIOS rebooting with it
			startup.mode = RUN_BOOTOS;
			startup.info = (int)dbp_osimages.size();
			dbp_osimages.emplace_back(filename);
		}

		const bool have_cd_image = BatchFileBoot::HaveCDImage();
		if (!path.empty())
		{
			// When booting an external disk image as C:, use whatever is C: in DOSBox DOS as the second hard disk in the booted OS (it being E: in Drives[] doesn't matter)
			char newC = ((have_cd_image || DBP_IsMounted('D')) ? 'E' : 'D'); // alternative would be to do DBP_Remount('D', 'E'); and always use 'D'
			if (imageDiskList['C'-'A'])
				imageDiskList[newC-'A'] = imageDiskList['C'-'A'];
			else if (!BatchFileBoot::MountOSIMG(newC, (dbp_content_path + ".img").c_str(), "D: drive image", true, false) && Drives['C'-'A'])
			{
				Bit32u save_hash = 0;
				DBP_SetDriveLabelFromContentPath(Drives['C'-'A'], dbp_content_path.c_str(), 'C', NULL, NULL, true);
				std::string save_path = DBP_GetSaveFile(SFT_VIRTUALDISK, NULL, &save_hash); // always call to fill out save_hash and dbp_vdisk_filter
				Bit32u freeSpace = (Bit32u)atoi(retro_get_variable("dosbox_pure_bootos_dfreespace", "1024")); // can be "discard"
				imageDiskList[newC-'A'] = new imageDisk(Drives['C'-'A'], (freeSpace ? freeSpace : 1024), (freeSpace ? save_path.c_str() : NULL) , save_hash, &dbp_vdisk_filter);
			}

			// Ramdisk setting must be false while installing os
			char ramdisk = (is_install ? 'f' : retro_get_variable("dosbox_pure_bootos_ramdisk", "false")[0]);

			// Now mount OS hard disk image as C: drive
			if (BatchFileBoot::MountOSIMG('C', path.c_str(), "OS image", (ramdisk == 'f'), true) && ramdisk == 'd')
				imageDiskList['C'-'A']->SetDifferencingDisk(DBP_GetSaveFile(SFT_DIFFDISK).c_str());
		}
		else if (!imageDiskList['C'-'A'] && Drives['C'-'A'])
		{
			// Running without hard disk image uses the DOS C: drive as a read-only C: hard disk
			imageDiskList['C'-'A'] = new imageDisk(Drives['C'-'A'], 0);
		}

		// Try reading a boot disk image off from an ISO file
		Bit8u* bootdisk_image; Bit32u bootdisk_size;
		if (!Drives['A'-'A'] && Drives['D'-'A'] && dynamic_cast<isoDrive*>(Drives['D'-'A']) && ((isoDrive*)(Drives['D'-'A']))->CheckBootDiskImage(&bootdisk_image, &bootdisk_size))
		{
			Drives['Y'-'A'] = new memoryDrive();
			DriveCreateFile(Drives['Y'-'A'], "CDBOOT.IMG", bootdisk_image, bootdisk_size);
			free(bootdisk_image);
			DBP_Mount(DBP_AppendImage("$Y:\\CDBOOT.IMG", false), true); // this mounts the image as the A drive
			//// Generate autoexec bat that starts the OS setup program?
			//DriveCreateFile(Drives['A'-'A'], "CONFIG.SYS", (const Bit8u*)"", 0);
			//DriveCreateFile(Drives['A'-'A'], "AUTOEXEC.BAT", (const Bit8u*)"DIR\r\n", 5);
		}

		// Setup IDE controllers for the hard drives and one CDROM drive (if any CDROM image is mounted)
		IDE_SetupControllers(have_cd_image ? 'D' : 0);

		// Install the NE2000 network card
		NET_SetupEthernet();

		// Switch cputype to highest feature set (needed for Windows 9x) and increase real mode CPU cycles
		Section* section = control->GetSection("cpu");
		section->ExecuteDestroy(false);
		section->HandleInputline("cputype=pentium_slow");
		if (retro_get_variable("dosbox_pure_bootos_forcenormal", "false")[0] == 't') section->HandleInputline("core=normal");
		section->ExecuteInit(false);
		section->GetProp("cputype")->OnChangedByConfigProgram();
		if (dbp_content_year < 1993 && (CPU_CycleAutoAdjust || (CPU_AutoDetermineMode & (CPU_AUTODETERMINE_CYCLES|(CPU_AUTODETERMINE_CYCLES<<CPU_AUTODETERMINE_SHIFT))))) DBP_SetCyclesByYear(1993, 1993);

		RunBatchFile(new BatchFileBoot(!is_install ? 'C' : 'A'));
	}

	static void RunShell(int shellidx)
	{
		if (!Drives['C'-'A']) return;
		if (dbp_had_game_running) { DBP_OnBIOSReboot(); return; }
		dbp_had_game_running = true;

		unionDrive* base_drive = dynamic_cast<unionDrive*>(Drives['C'-'A']);
		if (!base_drive) return;
		std::string path = DBP_GetSaveFile(SFT_SYSTEMDIR).append(dbp_shellzips[shellidx]);
		FILE* zip_file_h = fopen_wrap(path.c_str(), "rb");
		if (!zip_file_h) { retro_notify(0, RETRO_LOG_ERROR, "Unable to open %s file: %s", "System Shell", path.c_str()); return; }
		base_drive->AddUnder(*new zipDrive(new rawFile(zip_file_h, false), false), true);

		const char* exes[] = { "C:\\WINDOWS.BAT", "C:\\AUTOEXEC.BAT", "C:\\WINDOWS\\WIN.COM" };
		for (const char* exe : exes)
			if (Drives['C'-'A']->FileExists(exe + 3))
				{ RunBatchFile(new BatchFileExec(exe)); return; }

		ConsoleClearScreen();
		Bit16u sz;
		DOS_WriteFile(STDOUT, (Bit8u*)"To auto run the shell, make sure one of these files exist:\r\n", &(sz = sizeof("To auto run the shell, make sure one of these files exist:\r\n")-1));
		for (const char* exe : exes) { DOS_WriteFile(STDOUT, (Bit8u*)"\r\n- ", &(sz = 4)); DOS_WriteFile(STDOUT, (Bit8u*)exe, &(sz = (Bit16u)strlen(exe))); }
		DOS_WriteFile(STDOUT, (Bit8u*)"\r\n\r\n", &(sz = 4));
		KEYBOARD_AddKey(KBD_enter, true);
		KEYBOARD_AddKey(KBD_enter, false);
	}

	enum EMode : Bit8u { RUN_NONE, RUN_EXEC, RUN_BOOTIMG, RUN_BOOTOS, RUN_INSTALLOS, RUN_SHELL, RUN_VARIANT, RUN_COMMANDLINE };
	static struct Startup { EMode mode; bool reboot; int info; std::string exec; } startup;
	static struct Autoboot { Startup startup; bool have, use; Bit16s var; int skip; Bit32u hash; } autoboot;
	static struct Autoinput { std::string str; const char* ptr; Bit32s oldcycles; Bit8u oldchange; Bit16s oldyear; } autoinput;

	static bool Run(EMode mode, int info, std::string& str, bool from_osd = false)
	{
		if (from_osd)
		{
			WriteAutoBoot(mode, info, str);
			autoinput.str.clear();
		}

		if (mode == RUN_VARIANT)
		{
			startup.reboot |= patchDrive::SwitchVariant(info);
			startup.mode = RUN_NONE;
			DOSYML::Load(true, false); // read startup and autoinput from YML
			if ((mode = startup.mode) == RUN_NONE) return false; // YML had no startup
		}
		else
		{
			startup.reboot |= ((patchDrive::ActiveVariantIndex == -1 && patchDrive::Variants.size() && patchDrive::SwitchVariant(autoboot.var)) || startup.reboot);
			startup.mode = mode;
			startup.info = info;
			if (mode == RUN_EXEC) startup.exec.swap(str); // remember to set cursor again and for rebooting a different IT_RUN
		}

		if (startup.reboot || dbp_game_running || (mode == RUN_BOOTIMG && info && info != GetDosBoxMachineChar()))
		{
			startup.reboot = false;
			if (mode == RUN_BOOTIMG) dbp_reboot_machine = (info ? (char)info : GetDosBoxMachineChar());
			DBP_OnBIOSReboot();
			return true;
		}

		if (autoboot.use && autoboot.skip)
		{
			autoinput.str.assign(31, ' ');
			autoinput.str.resize(sprintf(&autoinput.str[0], "(WAIT:%d)", autoboot.skip * 15));
		}

		autoinput.ptr = ((mode != RUN_COMMANDLINE && autoinput.str.size()) ? autoinput.str.c_str() : NULL);
		autoinput.oldcycles = 0;
		if (autoinput.ptr && dbp_content_year > 1970 && (CPU_CycleAutoAdjust || (CPU_AutoDetermineMode & (CPU_AUTODETERMINE_CYCLES|(CPU_AUTODETERMINE_CYCLES<<CPU_AUTODETERMINE_SHIFT)))))
		{
			// enforce cycle rate during auto input (but limited to 1994 CPU speed, above will likely just waste time waiting for rendering out the skipped frames)
			autoinput.oldcycles = CPU_CycleMax;
			autoinput.oldchange = (Bit8u)control->GetSection("cpu")->GetProp("cycles")->getChange();
			autoinput.oldyear = dbp_content_year;
			if (dbp_content_year > 1994) dbp_content_year = 1994;
			DBP_SetCyclesByYear(dbp_content_year, 1994);
		}

		// if a booted OS does a bios reboot, auto reboot that OS from now on
		if (mode == RUN_EXEC || mode == RUN_COMMANDLINE)
			startup.mode = RUN_NONE;

		if (mode == RUN_EXEC)
			Exec(startup.exec);
		else if (mode == RUN_BOOTIMG)
			BootImage();
		else if (mode == RUN_BOOTOS || mode == RUN_INSTALLOS)
			BootOS(mode == RUN_INSTALLOS, startup.info);
		else if (mode == RUN_SHELL)
			RunShell(startup.info);
		return true;
	}

	static char GetDosBoxMachineChar() { return *((const char*)control->GetSection("dosbox")->GetProp("machine")->GetValue()); }

	struct DOSYML
	{
		const char *Key, *End, *Next, *KeyX, *Val, *ValX;
		int cpu_cycles = 0, cpu_hz = 0, cpu_year = 0, cpu_set_max = 0;
		bool Parse(const char *yml_key, const char* db_section, const char* db_key, ...)
		{
			if (yml_key && (strncmp(yml_key, Key, (size_t)(KeyX - Key)) || yml_key[KeyX - Key])) return false;
			va_list ap; va_start(ap, db_key); std::string val;
			for (;;)
			{
				const char* mapFrom = va_arg(ap, const char*);
				if (!*mapFrom) { va_end(ap); return false; }
				if (*mapFrom == '~')
				{
					val.append(Val, (size_t)(ValX - Val));
				}
				else if (*mapFrom == '/')
				{
					char buf[32];
					sprintf(buf, "%d", (atoi(Val) / 1024));
					val = buf;
				}
				else if (*mapFrom == '^')
				{
					const char* p = dbp_content_path.c_str(), *fs = strrchr(p, '/'), *bs = strrchr(p, '\\');
					(((val += '^') += (yml_key[7] == 't' ? 'M' : 'S')).append(p, (fs > bs ? (fs - p) : bs ? (bs - p) : 0)) += CROSS_FILESPLIT).append(Val, (size_t)(ValX - Val));
					Property* prop = control->GetSection("midi")->GetProp("midiconfig");
					prop->SetValue(val);
					prop->OnChangedByConfigProgram();
					val.assign("intelligent");
				}
				else
				{
					const char* mapTo = va_arg(ap, const char*);
					if (strncmp(mapFrom, Val, (size_t)(ValX - Val))) continue;
					val.append(mapTo);
				}
				Property* prop = control->GetSection(db_section)->GetProp(db_key);
				bool res = (prop->SetValue(val) && !strcasecmp(prop->GetValue().ToString().c_str(), val.c_str()));
				if (res) prop->OnChangedByConfigProgram();
				va_end(ap);
				return res;
			}
		}
		bool ParseCPU(const char *yml_key)
		{
			if (strncmp(yml_key, Key, (size_t)(KeyX - Key)) || yml_key[KeyX - Key]) return false;
			again: switch (yml_key[4])
			{
				case 'm': cpu_set_max = 1; yml_key += 4; goto again; // cpu_max_*
				case 'c': return ((cpu_cycles = atoi(Val)) >=  100); // cpu_cycles
				case 'h': return ((cpu_hz     = atoi(Val)) >=  500); // cpu_hz
				case 'y': return ((cpu_year   = atoi(Val)) >= 1970); // cpu_year
			}
			return false;
		}
		bool ParseRun(const char *yml_key)
		{
			if (strncmp(yml_key, Key, (size_t)(KeyX - Key)) || yml_key[KeyX - Key]) return false;
			switch (yml_key[4])
			{
				case 'i': // run_input
					autoinput.ptr = NULL;
					autoinput.str.clear();
					autoinput.str.append(Val, (size_t)(ValX - Val));
					break;
				case 'p': // run_path
					startup.exec = std::string(Val, (size_t)(ValX - Val));
					if (startup.mode == RUN_BOOTIMG) goto exec2bootimg;
					startup.mode = RUN_EXEC;
					break;
				case 'b': // run_boot
				case 'm': // run_mount
					{
						int imgidx = -1;
						for (DBP_Image& i : dbp_images)
							if ((i.path.size() == 4+(ValX - Val) && i.path[0] == '$' && !strncasecmp(&i.path[4], Val, (ValX - Val)))
								|| (i.longpath.size() == (ValX - Val) &&  !strncasecmp(&i.longpath[0], Val, (ValX - Val))))
								{ imgidx = (int)(&i - &dbp_images[0]); break; }
						if (imgidx == -1) return false;
						dbp_images[imgidx].remount = true;
					}
					if (yml_key[4] == 'm') break; // run_mount
					if (startup.mode == RUN_EXEC)
					{
						exec2bootimg:
						((static_cast<Section_line*>(control->GetSection("autoexec"))->data += '@') += startup.exec) += '\n';
					}
					startup.mode = RUN_BOOTIMG;
					startup.info = 0;
					break;
			}
			return true;
		}
		static void Load(bool parseRun, bool parseOthers)
		{
			const std::string& yml = patchDrive::DOSYMLContent; DOSYML l;
			for (l.Key = yml.c_str(), l.End = l.Key+yml.size(); l.Key < l.End; l.Key = l.Next + 1)
			{
				for (l.Next = l.Key; *l.Next != '\n' && *l.Next != '\r' && *l.Next; l.Next++) {}
				if (l.Next == l.Key || *l.Key == '#') continue;
				for (l.KeyX = l.Key; *l.KeyX && *l.KeyX != ':' && *l.KeyX > ' '; l.KeyX++) {}
				if (*l.KeyX != ':' || l.KeyX == l.Key || l.KeyX[1] != ' ' ) goto syntaxerror;
				for (l.Val = l.KeyX + 2; *l.Val == ' '; l.Val++) {}
				for (l.ValX = l.Val; *l.ValX && *l.ValX != '\r' && *l.ValX != '\n' && (*l.ValX != '#' || l.ValX[-1] != ' '); l.ValX++) {}
				while (l.ValX[-1] == ' ') l.ValX--;
				if (l.ValX <= l.Val) goto syntaxerror;
				switch (*l.Key)
				{
					case 'c':
						if (!parseOthers
							||l.Parse("cpu_type", "cpu", "cputype" , "auto","auto" , "generic_386","386" , "generic_486","486_slow" , "generic_pentium","pentium_slow" , "")
							||l.ParseCPU("cpu_cycles")||l.ParseCPU("cpu_hz")||l.ParseCPU("cpu_year")||l.ParseCPU("cpu_max_cycles")||l.ParseCPU("cpu_max_hz")||l.ParseCPU("cpu_max_year")
						) break; else goto syntaxerror;
					case 'm':
						if (!parseOthers
							||l.Parse("mem_size", "dosbox", "memsize", "/")
							||l.Parse("mem_xms", "dos", "xms" , "true","true" , "false","false" , "")
							||l.Parse("mem_ems", "dos", "ems" , "true","true" , "false","false" , "")
							||l.Parse("mem_umb", "dos", "umb" , "true","true" , "false","false" , "")
							||l.Parse("mem_doslimit", "dos", "memlimit", "~")
						) break; else goto syntaxerror;
					case 'v':
						if (!parseOthers
							||l.Parse("video_card", "dosbox", "machine" , "generic_svga","svga_s3" , "generic_hercules","hercules" , "generic_cga","cga" , "generic_tandy","tandy" , "generic_pcjr","pcjr" , "generic_ega","ega" , "generic_vga","vgaonly" , "svga_s3_trio","svga_s3", "svga_tseng_et3000","svga_et3000" , "svga_tseng_et4000","svga_et4000" , "svga_paradise_pvga1a","svga_paradise" , "")
							||l.Parse("video_memory", "dosbox", "vmemsize", "/")
							||l.Parse("video_voodoo", "pci", "voodoo" , "true","12mb" , "false","false" , "")
						) break; else goto syntaxerror;
					case 's':
						if (!parseOthers
							||l.Parse("sound_card", "sblaster", "sbtype" , "sb16","sb16" , "sb1","sb1" , "sb2","sb2" , "sbpro1","sbpro1" , "sbpro2","sbpro2" , "gameblaster","gb" , "none","none" , "")
							||l.Parse("sound_port", "sblaster", "sbbase" , "~")
							||l.Parse("sound_irq", "sblaster", "irq", "~")
							||l.Parse("sound_dma", "sblaster", "dma", "~")
							||l.Parse("sound_hdma", "sblaster", "hdma", "~")
							||l.Parse("sound_midi", "midi", "mpu401" , "true","intelligent" , "false","none" , "^")
							||l.Parse("sound_mt32", "midi", "mpu401" , "true","intelligent" , "false","none" , "^")
							||l.Parse("sound_gus", "gus", "gus" , "true","true" , "false","false" , "")
							||l.Parse("sound_tandy", "speaker", "tandy" , "true","on" , "false","auto" , "")
						) break; else goto syntaxerror;
					case 'r':
						if (!parseRun
							||l.ParseRun("run_path")
							||l.ParseRun("run_boot")
							||l.ParseRun("run_mount")
							||l.ParseRun("run_input")
						) break; else goto syntaxerror;
				}
				continue;
				syntaxerror:
				retro_notify(0, RETRO_LOG_ERROR, "Error in DOS.YML: %.*s", (int)(l.Next-l.Key), l.Key);
				continue;
			}
			if (l.cpu_cycles || l.cpu_year || l.cpu_hz)
			{
				if (l.cpu_cycles) {}
				else if (l.cpu_year) l.cpu_cycles = (int)DBP_CyclesForYear(l.cpu_year);
				else
				{
					float cycle_per_hz = .3f; // default with auto (just a bad guess)
					switch (*(const char*)control->GetSection("cpu")->GetProp("cputype")->GetValue())
					{
						case 'p': cycle_per_hz = .55700f; break; // Pentium (586):  Mhz * 557.00
						case '4': cycle_per_hz = .38000f; break; // 486:            Mhz * 380.00
						case '3': cycle_per_hz = .18800f; break; // 386:            Mhz * 188.00
						case '2': cycle_per_hz = .09400f; break; // AT (286):       Mhz *  94.00
						case '8': cycle_per_hz = .05828f; break; // XT (8088/8086): Mhz *  58.28
					}
					l.cpu_cycles = (int)(l.cpu_hz * cycle_per_hz + .4999f);
				}
				char buf[32];
				l.ValX = (l.Val = buf) + sprintf(buf, "%s%d", (l.cpu_set_max ? "max limit " : ""), (int)l.cpu_cycles);
				if (l.Parse(NULL, "cpu", "cycles", "~") && l.cpu_cycles >= 8192) // Switch to dynamic core for newer real mode games
					{ l.ValX = (l.Val = "dynamic") + 7; l.Parse(NULL, "cpu", "core", "~"); }
			}
		}
	};

	static void PreInit()
	{
		if (!dbp_biosreboot) startup.mode = RUN_NONE;
		if (patchDrive::DOSYMLContent.size()) DOSYML::Load((!dbp_biosreboot && (patchDrive::Variants.size() == 1 || autoboot.startup.mode == RUN_VARIANT)), true); // ignore run keys on bios reboot
		if (!dbp_biosreboot && autoboot.use && autoboot.startup.mode != RUN_VARIANT) startup = autoboot.startup;
	}

	static bool PostInitFirstTime()
	{
		ReadAutoBoot();
		int switchVariant = (patchDrive::Variants.size() ? autoboot.var : -1);
		return (switchVariant != -1 && patchDrive::SwitchVariant(switchVariant));
	}

	static void ReadAutoBoot()
	{
		char buf[DOS_PATHLENGTH + 32 + 256 + 256 + 2];
		Bit16u autostrlen = DriveReadFileBytes(Drives['C'-'A'], "AUTOBOOT.DBP", (Bit8u*)buf, (Bit16u)(sizeof(buf)-1));
		autoboot.have = !!autostrlen;

		const char* cpath = (autostrlen ? NULL : strrchr(dbp_content_path.c_str(), '#'));
		if (cpath && (dbp_content_path.c_str() + dbp_content_path.length() - cpath) <= DOS_PATHLENGTH)
			autostrlen = (Bit16u)sprintf(buf, "%s%s", (cpath[1] && cpath[2] == ':' ? "" : "C:\\"), cpath + 1);

		for (char *p = buf, *pEnd = p + autostrlen, *line, line_no = 1; p != pEnd; line_no++)
		{
			if ((p += (!*p ? (!p[1] ? 2 : 1) : 0)) >= pEnd) break;
			for (line = p; p != pEnd && *p >= ' ';) p++;
			if (*p == '\r' && p[1] == '\n') p[1] = '\0';
			*p = '\0'; // for strcmp/atoi/DOS_FileExists/assign
			if (line_no == 1)
			{
				const char linetype = (line[1] == '*' ? line[0] : 0), *str = line + (linetype ? 2 : 0);
				if (linetype == 0)
				{
					if (DOS_FileExists(str)) { autoboot.startup.mode = RUN_EXEC; autoboot.startup.exec.assign(str); }
				}
				else if (linetype == 'O' || linetype == 'S' || linetype == 'V')
				{
					EMode mode                     = (linetype == 'O' ?   RUN_BOOTOS : (linetype == 'O' ?     RUN_SHELL :          RUN_VARIANT));
					size_t suffix_len              = (linetype == 'O' ?             4: (linetype == 'O' ?             5 :                    0));
					std::vector<std::string>& strs = (linetype == 'O' ? dbp_osimages : (linetype == 'O' ? dbp_shellzips : patchDrive::Variants));
					for (const std::string& it : strs)
						if (it.size() == (p - str) + suffix_len && !memcmp(str, it.c_str(), it.size() - suffix_len))
							{ autoboot.startup.mode = mode; autoboot.startup.info = (int)(&it - &strs[0]); break; }
				}
				else if (linetype == 'I')
				{
					for (const char* it : DBP_MachineNames)
						if (!strcmp(it, str))
							{ autoboot.startup.mode = RUN_BOOTIMG; autoboot.startup.info = (Bit16s)(it[0]|0x20); break; }
				}
			}
			else if (line_no == 2)
			{
				autoboot.skip = atoi(line);
			}
			else if (line_no == 3 && *line)
			{
				for (const DBP_Image& i : dbp_images)
					if (!strcmp(DBP_Image_Label(i), line))
						{ if (!i.mounted) DBP_Mount((unsigned)(&i - &dbp_images[0]), true); break; }
			}
			else if (line_no == 4)
			{
				for (const std::string& it : patchDrive::Variants)
					if (it.size() == (p - line) && !memcmp(line, it.c_str(), it.size()))
						{ autoboot.var = (Bit16s)(&it - &patchDrive::Variants[0]); break; }
			}
		}
		if (autoboot.startup.mode == RUN_VARIANT) { autoboot.skip = 0; autoboot.var = (Bit16s)autoboot.startup.info; }
		autoboot.use = (autoboot.startup.mode != RUN_NONE);
		autoboot.hash = HashAutoBoot();
	}

	static void WriteAutoBoot(EMode mode, int info, const std::string& str)
	{
		if ((!autoboot.use || mode == RUN_NONE || mode == RUN_INSTALLOS || mode == RUN_COMMANDLINE) && mode != RUN_VARIANT) // force enable auto start when switching variant
		{
			if (autoboot.have) Drives['C'-'A']->FileUnlink((char*)"AUTOBOOT.DBP");
			autoboot.startup.mode = RUN_NONE; autoboot.have = autoboot.use = false;
			return;
		}
		DBP_ASSERT(mode == RUN_EXEC || mode == RUN_BOOTOS || mode == RUN_SHELL || mode == RUN_VARIANT || mode == RUN_BOOTIMG);
		autoboot.startup.mode = mode;
		autoboot.startup.info = info;
		autoboot.startup.exec.assign(mode == RUN_EXEC ? str.c_str() : "");
		autoboot.var = patchDrive::ActiveVariantIndex;
		if (mode == RUN_VARIANT) { autoboot.use = true; autoboot.skip = 0; autoboot.var = (Bit16s)info; }
		if (HashAutoBoot() == autoboot.hash) return;
		autoboot.hash = HashAutoBoot();
		autoboot.have = true;
		const char *img = NULL, *var = NULL;
		for (const DBP_Image& i : dbp_images) { if (i.mounted) { if (&i != &dbp_images[0]) img = DBP_Image_Label(i); break; } }
		if (patchDrive::ActiveVariantIndex > 0 && mode != RUN_VARIANT) var = patchDrive::Variants[patchDrive::ActiveVariantIndex].c_str();
		char buf[DOS_PATHLENGTH + 32 + 256 + 256], *p = buf;
		if (mode != RUN_EXEC) { *(p++) = (mode == RUN_BOOTOS ? 'O' : mode == RUN_SHELL ? 'S' : mode == RUN_VARIANT ? 'V' : 'I'); *(p++) = '*'; }
		if (1)                           p += snprintf(p, (&buf[sizeof(buf)] - p), "%s", str.c_str());          // line 1
		if (var || img || autoboot.skip) p += snprintf(p, (&buf[sizeof(buf)] - p), "\r\n%d", autoboot.skip);    // line 2
		if (var || img)                  p += snprintf(p, (&buf[sizeof(buf)] - p), "\r\n%s", (img ? img : "")); // line 3
		if (var)                         p += snprintf(p, (&buf[sizeof(buf)] - p), "\r\n%s", var);              // line 4
		if (!DriveCreateFile(Drives['C'-'A'], "AUTOBOOT.DBP", (Bit8u*)buf, (Bit32u)(p - buf))) { DBP_ASSERT(false); }
	}

	static Bit32u HashAutoBoot() { return DriveCalculateCRC32((Bit8u*)&autoboot, sizeof(autoboot), BaseStringToPointerHashMap::Hash(autoboot.startup.exec.c_str())); } 

	static Bit32u ModeHash() { return (Bit32u)((render.src.width * 2100781) ^ (render.src.height * 65173) ^ ((Bitu)(render.src.fps * 521)) ^ (render.src.bpp * 31) ^ ((Bitu)vga.mode + 1)); }

	static void ProcessAutoInput()
	{
		extern Bitu PIC_Ticks;
		static Bitu InpTickStart, InpNextTick; static Bit32u InpDelay, InpReleaseKey, InpSkipMode;
		if (autoinput.ptr == autoinput.str.c_str())
			InpTickStart = PIC_Ticks, InpNextTick = 0, InpDelay = 70, InpReleaseKey = InpSkipMode = 0;

		const Bitu InpDoneTicks = PIC_Ticks - InpTickStart;
		if (InpSkipMode && !vga.draw.resizing)
		{
			Bit32u mode = ModeHash();
			if (InpSkipMode == mode) { } // video mode unchanged
			else if (InpSkipMode < 31) { InpSkipMode = mode; } // initial resolution was set (before it was size 0)
			else { InpSkipMode = 0; InpNextTick = InpDoneTicks; } // new video mode
		}

		while (InpDoneTicks >= InpNextTick)
		{
			if (InpReleaseKey)
			{
				if (InpReleaseKey & 0x100) { KEYBOARD_AddKey(KBD_rightalt, false); InpReleaseKey &= 0xFF; }
				if (InpReleaseKey & 0x80) { KEYBOARD_AddKey(KBD_leftshift, false); InpReleaseKey &= 0x7F; }
				KEYBOARD_AddKey((KBD_KEYS)InpReleaseKey, false);
				InpReleaseKey = 0;
				if (*autoinput.ptr) { InpNextTick += InpDelay; continue; }
			}
			if (!*autoinput.ptr) { autoinput.ptr = NULL; break; }

			const char *cmd = autoinput.ptr, *cmdNext = cmd + 1, *cmdColon = NULL;
			bool bShift = false, bAltGr = false;
			char tmp;
			Bit32u i = 0, cmdlen = 1;

			if (cmd[0] != '(' || cmd[1] == '(')
			{
				if (!(cmd[0] != '(')) cmdNext++; // Treat (( as textinput (
				KBD_KEYS mappedkey = KBD_NONE;
				char DBP_DOS_KeyboardLayout_MapChar(char c, bool& bShift, bool& bAltGr);
				switch ((tmp = DBP_DOS_KeyboardLayout_MapChar(cmd[0], bShift, bAltGr)))
				{
					case '\x1B': i = KBD_esc;          break;
					case '-':    i = KBD_minus;        break;
					case '=':    i = KBD_equals;       break;
					case '\b':   i = KBD_backspace;    break;
					case '\t':   i = KBD_tab;          break;
					case '[':    i = KBD_leftbracket;  break;
					case ']':    i = KBD_rightbracket; break;
					case ';':    i = KBD_semicolon;    break;
					case '\'':   i = KBD_quote;        break;
					case '`':    i = KBD_grave;        break;
					case '\\':   i = KBD_backslash;    break;
					case ',':    i = KBD_comma;        break;
					case '.':    i = KBD_period;       break;
					case '/':    i = KBD_slash;        break;
					default: cmd = &tmp;
				}
			}
			else if ((cmdNext = strchr(cmdNext, ')')) != NULL)
			{
				if ((cmdColon = strchr(++cmd, ':')) != NULL && cmdColon >= cmdNext-1) cmdColon = NULL;
				cmdlen = (Bit32u)((cmdColon ? cmdColon : cmdNext) - cmd);
				cmdNext++;
			}

			static const char* DBP_Commands[KBD_LAST+3] =
			{
				"","1","2","3","4","5","6","7","8","9","0","q","w","e","r","t","y","u","i","o","p","a","s","d","f","g","h","j","k","l","z","x","c","v","b","n","m",
				"f1","f2","f3","f4","f5","f6","f7","f8","f9","f10","f11","f12","esc","tab","backspace","enter","space","leftalt","rightalt","leftctrl","rightctrl","leftshift","rightshift",
				"capslock","scrolllock","numlock","grave","minus","equals","backslash","leftbracket","rightbracket","semicolon","quote","period","comma","slash","extra_lt_gt",
				"printscreen","pause","insert","home","pageup","delete","end","pagedown","left","up","down","right","kp1","kp2","kp3","kp4","kp5","kp6","kp7","kp8","kp9","kp0",
				"kpdivide","kpmultiply","kpminus","kpplus","kpenter","kpperiod",
				"wait","waitmodechange","delay"
			};
			if (i == 0)
				for (; i != KBD_LAST+3; i++)
					if (!strncasecmp(DBP_Commands[i], cmd, cmdlen) && DBP_Commands[i][cmdlen] == '\0')
						break;

			if (i == KBD_LAST+0 && cmdColon) // wait command
			{
				InpNextTick += atoi(cmdColon+1);
			}
			else if (i == KBD_LAST+1) // waitmodechange command
			{
				if (vga.draw.resizing && InpDoneTicks && (InpDoneTicks - InpNextTick < 5000)) break; // don't start while vga is resizing
				InpNextTick += 30000; // wait max 30 seconds (if the game crashes, auto input is aborted below)
				InpSkipMode = ModeHash();
			}
			else if (i == KBD_LAST+2 && cmdColon) // delay command
			{
				InpDelay = (Bit32u)atoi(cmdColon+1);
			}
			else if (i < KBD_LAST && cmdColon && (!strncasecmp(cmdColon+1, "down", 4) || strncasecmp(cmdColon+1, "up", 2))) // key command
			{
				KEYBOARD_AddKey((KBD_KEYS)i, (cmdColon[1]|0x20) == 'd');
			}
			else if (i < KBD_LAST) // key press
			{
				if (bShift) KEYBOARD_AddKey(KBD_leftshift, true);
				if (bAltGr) KEYBOARD_AddKey(KBD_rightalt, true);
				KEYBOARD_AddKey((KBD_KEYS)i, true);
				InpReleaseKey = (i | (bShift ? 0x80 : 0) | (bAltGr ? 0x100 : 0));
				InpNextTick += 70; // fixed press duration
			}
			else
			{
				log_cb(RETRO_LOG_INFO, "[DOSBOX ERROR] Unknown command in run_input string: '%s'\n", cmd);
				autoinput.ptr = NULL;
				break;
			}
			autoinput.ptr = cmdNext;
		}

		// Check if done, dbp_game_running should switch to true at tick 1 unless the game crashes or exits but give it 5 seconds to be sure
		if (autoinput.ptr && (dbp_game_running || InpDoneTicks < 5000))
		{
			// Disable line rendering (without using VGA frameskipping which affects the emulation)
			struct Local { static void EmptyLineHandler(const void*) { } };
			RENDER_DrawLine = Local::EmptyLineHandler;
			// Scrap mixed audio instead of accumulating it while skipping frames
			int mixSamples = (int)DBP_MIXER_DoneSamplesCount();
			if (mixSamples > DBP_MAX_SAMPLES) mixSamples = DBP_MAX_SAMPLES;
			if (mixSamples > 200) MIXER_CallBack(0, (Bit8u*)dbp_audio, (mixSamples - 100) * 4);
		}
		else
		{
			// done
			autoinput.ptr = NULL; // reset on game crash/exit (dbp_game_running is false)
			DBP_KEYBOARD_ReleaseKeys();
			if (autoinput.oldcycles)
			{
				if (!CPU_CycleAutoAdjust && CPU_CycleMax == DBP_CyclesForYear(dbp_content_year, 1994) && control->GetSection("cpu")->GetProp("cycles")->getChange() == autoinput.oldchange)
					CPU_CycleMax = autoinput.oldcycles; // revert from Run()
				else if (CPU_CycleAutoAdjust && cpu.pmode && (CPU_AutoDetermineMode & (CPU_AUTODETERMINE_CORE<<CPU_AUTODETERMINE_SHIFT)))
					CPU_OldCycleMax = autoinput.oldcycles; // we switched to protected mode since auto input, fix up old cycles
				dbp_content_year = autoinput.oldyear;
				DBP_SetRealModeCycles(); // if still in real mode reset the defaults
			}
		}
	}
};

DBP_Run::Startup DBP_Run::startup;
DBP_Run::Autoinput DBP_Run::autoinput;
DBP_Run::Autoboot DBP_Run::autoboot;
