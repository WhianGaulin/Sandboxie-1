/*
 *
 * Copyright (c) 2020, David Xanatos
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "stdafx.h"
#include <QtConcurrent>
#include "SandBox.h"
#include "../SbieAPI.h"

#include <ntstatus.h>
#define WIN32_NO_STATUS
typedef long NTSTATUS;

#include <windows.h>
#include "..\..\Sandboxie\common\win32_ntddk.h"

#include "Helpers/NtIO.h"

//struct SSandBox
//{
//};

CSandBox::CSandBox(const QString& BoxName, class CSbieAPI* pAPI) : CSbieIni(BoxName, pAPI)
{
	//m = new SSandBox;

	m_ActiveProcessCount = 0;

	// when loading a sandbox that is not initialized, initialize it
	int cfglvl = GetNum("ConfigLevel");
	if (cfglvl >= 7)
		return;
	SetNum("ConfigLevel", 7);

	if (cfglvl == 6) {
		//SetDefaultTemplates7(*this);
	}
	else if (cfglvl >= 1) {
		//UpdateTemplates(*this);
	}
	else
	{
		SetBool("AutoRecover", false);
		SetBool("BlockNetworkFiles", true);

		//SetDefaultTemplates6(*this); // why 6?

		InsertText("RecoverFolder", "%Desktop%");
		//InsertText("RecoverFolder", "%Favorites%"); // obsolete
		InsertText("RecoverFolder", "%Personal%");
		InsertText("RecoverFolder", "%{374DE290-123F-4565-9164-39C4925E467B}%"); // %USERPROFILE%\Downloads

		SetText("BorderColor", "#00FFFF,ttl"); // "#00FFFF,off"
	}
}

CSandBox::~CSandBox()
{
	//delete m;
}

void CSandBox::UpdateDetails()
{
}

SB_STATUS CSandBox::RunStart(const QString& Command)
{
	return m_pAPI->RunStart(m_Name, Command);
}

SB_STATUS CSandBox::RunCommand(const QString& Command)
{
	return m_pAPI->RunSandboxed(m_Name, Command);
}

SB_STATUS CSandBox::TerminateAll()
{
	return m_pAPI->TerminateAll(m_Name);
}

SB_PROGRESS CSandBox::CleanBox()
{
	if (GetBool("NeverDelete", false))
		return SB_ERR(tr("Delete protection is enabled for the sandbox"));

	SB_STATUS Status = TerminateAll();
	if (Status.IsError())
		return Status;

	return CleanBoxFolders(QStringList(m_FilePath));
}

SB_PROGRESS CSandBox::CleanBoxFolders(const QStringList& BoxFolders)
{
	CSbieProgressPtr pProgress = CSbieProgressPtr(new CSbieProgress());
	QtConcurrent::run(CSandBox::CleanBoxAsync, pProgress, BoxFolders);
	return SB_PROGRESS(OP_ASYNC, pProgress);
}

SB_STATUS CSandBox__DeleteFolder(const CSbieProgressPtr& pProgress, const QString& Folder)
{
	if (!QDir().exists(Folder))
		return SB_OK;

	pProgress->ShowMessage(CSandBox::tr("Waiting for folder: %1").arg(Folder));

	SNtObject ntObject(L"\\??\\" + Folder.toStdWString());

	NtIo_WaitForFolder(&ntObject.attr);

	if (pProgress->IsCancel())
		return STATUS_REQUEST_ABORTED; // or STATUS_TRANSACTION_ABORTED ?

	pProgress->ShowMessage(CSandBox::tr("Deleting folder: %1").arg(Folder));

	NTSTATUS status = NtIo_DeleteFolderRecursively(&ntObject.attr);
	if (!NT_SUCCESS(status))
		return SB_ERR(CSandBox::tr("Error deleting sandbox folder: %1").arg(Folder), status);
	return SB_OK;
}

void CSandBox::CleanBoxAsync(const CSbieProgressPtr& pProgress, const QStringList& BoxFolders)
{
	SB_STATUS Status;

	foreach(const QString& Folder, BoxFolders)
	{
		Status = CSandBox__DeleteFolder(pProgress, Folder);
		if (Status.IsError())
			break;
	}

	pProgress->Finish(Status);
}

SB_STATUS CSandBox::RenameBox(const QString& NewName)
{
	if (QDir(m_FilePath).exists())
		return SB_ERR(tr("A sandbox must be emptied before it can be renamed."));
	if(NewName.length() > 32)
		return SB_ERR(tr("The sandbox name can not be longer than 32 charakters."));
	
	return RenameSection(QString(NewName).replace(" ", "_"));
}

SB_STATUS CSandBox::RemoveBox()
{
	if (QDir(m_FilePath).exists())
		return SB_ERR(tr("A sandbox must be emptied before it can be deleted."));

	return RemoveSection();
}

QList<SBoxSnapshot> CSandBox::GetSnapshots(QString* pCurrent) const
{
	QSettings ini(m_FilePath + "\\Snapshots.ini", QSettings::IniFormat);

	QList<SBoxSnapshot> Snapshots;

	foreach(const QString& Snapshot, ini.childGroups())
	{
		if (Snapshot.indexOf("Snapshot_") != 0)
			continue;

		SBoxSnapshot BoxSnapshot;
		BoxSnapshot.ID = Snapshot.mid(9);
		BoxSnapshot.Parent = ini.value(Snapshot + "/Parent").toString();

		BoxSnapshot.NameStr = ini.value(Snapshot + "/Name").toString();
		BoxSnapshot.InfoStr = ini.value(Snapshot + "/Description").toString();
		BoxSnapshot.SnapDate = ini.value(Snapshot + "/SnapshotDate").toDateTime();

		Snapshots.append(BoxSnapshot);
	}

	if(pCurrent)
		*pCurrent = ini.value("Current/Snapshot").toString();

	return Snapshots;
}

QStringList CSandBox__BoxSubFolders = QStringList() << "drive" << "user" << "share";

SB_STATUS CSandBox__MoveFolder(const QString& SourcePath, const QString& ParentFolder, const QString& TargetName)
{
	SNtObject src_dir(L"\\??\\" + SourcePath.toStdWString());
	SNtObject dest_dir(L"\\??\\" + ParentFolder.toStdWString());
	NTSTATUS status = NtIo_RenameFolder(&src_dir.attr, &dest_dir.attr, TargetName.toStdWString().c_str());
	if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_NOT_FOUND && status != STATUS_OBJECT_PATH_NOT_FOUND)
		return SB_ERR(CSandBox::tr("Failed to move directory '%1' to '%2'").arg(SourcePath).arg(ParentFolder + "\\" + TargetName), status);
	return SB_OK;
}

SB_PROGRESS CSandBox::TakeSnapshot(const QString& Name)
{
	QSettings ini(m_FilePath + "\\Snapshots.ini", QSettings::IniFormat);

	if (m_pAPI->HasProcesses(m_Name))
		return SB_ERR(tr("Can't take a snapshot while processes are running in the box."), OP_CONFIRM);

	QStringList Snapshots = ini.childGroups();

	QString ID;
	for (int i = 1; ; i++)
	{
		ID = QString::number(i);
		if(!Snapshots.contains("Snapshot_" + ID))
			break;
	}

	if (!QDir().mkdir(m_FilePath + "\\snapshot-" + ID))
		return SB_ERR(tr("Failed to create directory for new snapshot"));
	if (!QFile::copy(m_FilePath + "\\RegHive", m_FilePath + "\\snapshot-" + ID + "\\RegHive"))
		return SB_ERR(tr("Failed to copy RegHive to snapshot"));

	ini.setValue("Snapshot_" + ID + "/Name", Name);
	ini.setValue("Snapshot_" + ID + "/SnapshotDate", QDateTime::currentDateTime());
	QString Current = ini.value("Current/Snapshot").toString();
	if(!Current.isEmpty())
		ini.setValue("Snapshot_" + ID + "/Parent", Current);

	ini.setValue("Current/Snapshot", ID);
	ini.sync();

	foreach(const QString& BoxSubFolder, CSandBox__BoxSubFolders) 
	{
		SB_STATUS Status = CSandBox__MoveFolder(m_FilePath + "\\" + BoxSubFolder, m_FilePath + "\\snapshot-" + ID, BoxSubFolder);
		if (Status.IsError())
			return Status;
	}
	return SB_OK;
}

SB_PROGRESS CSandBox::RemoveSnapshot(const QString& ID)
{
	QSettings ini(m_FilePath + "\\Snapshots.ini", QSettings::IniFormat);

	if (!ini.childGroups().contains("Snapshot_" + ID))
		return SB_ERR(tr("Snapshot not found"));

	if (m_pAPI->HasProcesses(m_Name))
		return SB_ERR(tr("Can't remove a snapshots while processes are running in the box."), OP_CONFIRM);
	
	QStringList ChildIDs;
	foreach(const QString& Snapshot, ini.childGroups())
	{
		if (Snapshot.indexOf("Snapshot_") != 0)
			continue;

		if (ini.value(Snapshot + "/Parent").toString() == ID)
			ChildIDs.append(Snapshot.mid(9));
	}

	QString Current = ini.value("Current/Snapshot").toString();
	bool IsCurrent = Current == ID;

	if (ChildIDs.count() >= 2 || (ChildIDs.count() == 1 && IsCurrent))
		return SB_ERR(tr("Can't remove a snapshots that is shared by multiple later snapshots"));

	CSbieProgressPtr pProgress = CSbieProgressPtr(new CSbieProgress());
	if (ChildIDs.count() == 1 || IsCurrent)
		QtConcurrent::run(CSandBox::MergeSnapshotAsync, pProgress, m_FilePath, ID, IsCurrent ? QString() : ChildIDs.first());
	else
		QtConcurrent::run(CSandBox::DeleteSnapshotAsync, pProgress, m_FilePath, ID);
	return SB_PROGRESS(OP_ASYNC, pProgress);
}

void CSandBox::DeleteSnapshotAsync(const CSbieProgressPtr& pProgress, const QString& BoxPath, const QString& ID)
{
	SB_STATUS Status = CSandBox__DeleteFolder(pProgress, BoxPath + "\\snapshot-" + ID);

	if (!Status.IsError())
	{
		QSettings ini(BoxPath + "\\Snapshots.ini", QSettings::IniFormat);

		ini.remove("Snapshot_" + ID);
		ini.sync();
	}

	pProgress->Finish(Status);
}

SB_STATUS CSandBox__MergeFolders(const CSbieProgressPtr& pProgress, const QString& TargetFolder, const QString& SourceFolder)
{
	if (!QDir().exists(SourceFolder))
		return SB_OK; // nothing to do

	pProgress->ShowMessage(CSandBox::tr("Waiting for folder: %1").arg(SourceFolder));

	SNtObject ntSource(L"\\??\\" + SourceFolder.toStdWString());

	NtIo_WaitForFolder(&ntSource.attr);

	if (!QDir().exists(TargetFolder))
		QDir().mkpath(TargetFolder); // just make it
	
	pProgress->ShowMessage(CSandBox::tr("Waiting for folder: %1").arg(TargetFolder));

	SNtObject ntTarget(L"\\??\\" + TargetFolder.toStdWString());

	NtIo_WaitForFolder(&ntTarget.attr);

	if (pProgress->IsCancel())
		return STATUS_REQUEST_ABORTED; // or STATUS_TRANSACTION_ABORTED ?

	pProgress->ShowMessage(CSandBox::tr("Merging folders: %1 >> %2").arg(SourceFolder).arg(TargetFolder));

	NTSTATUS status = NtIo_MergeFolder(&ntSource.attr, &ntTarget.attr);
	if (!NT_SUCCESS(status))
		return SB_ERR(CSandBox::tr("Error merging snapshot directories '%1' with '%2', the snapshot has not been fully merged.").arg(TargetFolder).arg(SourceFolder), status);
	return SB_OK;
}

SB_STATUS CSandBox__CleanupSnapshot(const QString& Folder)
{
	SNtObject ntHiveFile(L"\\??\\" + (Folder + "\\RegHive").toStdWString());
	SB_STATUS status = NtDeleteFile(&ntHiveFile.attr);
	if (NT_SUCCESS(status)) {
		SNtObject ntSnapshotFile(L"\\??\\" + Folder.toStdWString());
		status = NtDeleteFile(&ntSnapshotFile.attr);
	}
	if (!NT_SUCCESS(status))
		return SB_ERR(CSandBox::tr("Failed to remove old snapshot directory '%1'").arg(Folder), status);
	return SB_OK;
}

void CSandBox::MergeSnapshotAsync(const CSbieProgressPtr& pProgress, const QString& BoxPath, const QString& TargetID, const QString& SourceID)
{
	//
	// Targe is to be removed;
	// Source is the child snpshot that has to remain
	// we merge target with source by overwrite target with source
	// than we rename target to source
	// finally we adapt the ini
	//

	bool IsCurrent = SourceID.isEmpty();
	QString SourceFolder = IsCurrent ? BoxPath : (BoxPath + "\\snapshot-" + SourceID);
	QString TargetFolder = BoxPath + "\\snapshot-" + TargetID;

	SB_STATUS Status = SB_OK;
	
	foreach(const QString& BoxSubFolder, CSandBox__BoxSubFolders) 
	{
		Status = CSandBox__MergeFolders(pProgress, TargetFolder + "\\" + BoxSubFolder, SourceFolder + "\\" + BoxSubFolder);
		if (Status.IsError())
			break;
	}

	pProgress->ShowMessage(CSandBox::tr("Finishing Snapshot Merge..."));

	if(!Status.IsError())
	{
		if (IsCurrent)
		{
			// move all folders out of the snapshot to root
			foreach(const QString& BoxSubFolder, CSandBox__BoxSubFolders) 
			{
				Status = CSandBox__MoveFolder(TargetFolder + "\\" + BoxSubFolder, SourceFolder, BoxSubFolder);
				if (Status.IsError())
					break;
			}

			// delete snapshot rest
			if (!Status.IsError())
				Status = CSandBox__CleanupSnapshot(TargetFolder);
		}
		else
		{
			// delete rest of source snpshot
			Status = CSandBox__CleanupSnapshot(SourceFolder);

			// rename target snapshot o source snapshot
			if (!Status.IsError())
				Status = CSandBox__MoveFolder(TargetFolder, BoxPath, "snapshot-" + SourceID);
		}
	}

	// save changes to the ini
	if (!Status.IsError())
	{
		QSettings ini(BoxPath + "\\Snapshots.ini", QSettings::IniFormat);

		QString TargetParent = ini.value("Snapshot_" + TargetID + "/Parent").toString();
		if (IsCurrent)
			ini.setValue("Current/Snapshot", TargetParent);
		else
			ini.setValue("Snapshot_" + SourceID + "/Parent", TargetParent);

		ini.remove("Snapshot_" + TargetID);
		ini.sync();
	}

	pProgress->Finish(Status);
}

SB_PROGRESS CSandBox::SelectSnapshot(const QString& ID)
{
	QSettings ini(m_FilePath + "\\Snapshots.ini", QSettings::IniFormat);

	if (!ini.childGroups().contains("Snapshot_" + ID))
		return SB_ERR(tr("Snapshot not found"));

	if (m_pAPI->HasProcesses(m_Name))
		return SB_ERR(tr("Can't switch snapshots while processes are running in the box."), OP_CONFIRM);

	if (!QFile::remove(m_FilePath + "\\RegHive"))
		return SB_ERR(tr("Failed to remove old RegHive"));
	if (!QFile::copy(m_FilePath + "\\snapshot-" + ID + "\\RegHive", m_FilePath + "\\RegHive"))
		return SB_ERR(tr("Failed to copy RegHive from snapshot"));

	ini.setValue("Current/Snapshot", ID);
	ini.sync();

	QStringList BoxFolders;
	foreach(const QString& BoxSubFolder, CSandBox__BoxSubFolders)
		BoxFolders.append(m_FilePath + "\\" + BoxSubFolder);
	return CleanBoxFolders(BoxFolders);
}

SB_STATUS CSandBox::SetSnapshotInfo(const QString& ID, const QString& Name, const QString& Description)
{
	QSettings ini(m_FilePath + "\\Snapshots.ini", QSettings::IniFormat);

	if (!ini.childGroups().contains("Snapshot_" + ID))
		return SB_ERR(tr("Snapshot not found"));

	if (!Name.isNull())
		ini.setValue("Snapshot_" + ID + "/Name", Name);
	if (!Description.isNull())
		ini.setValue("Snapshot_" + ID + "/Description", Description);

	return SB_OK;
}
