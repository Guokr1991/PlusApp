/*=Plus=header=begin======================================================
Program: Plus
Copyright (c) Laboratory for Percutaneous Surgery. All rights reserved.
See License.txt for details.
=========================================================Plus=header=end*/ 

#include "CapturingToolbox.h"

#include "fCalMainWindow.h"
#include "vtkVisualizationController.h"
#include "VolumeReconstructionToolbox.h"

#include "vtkDataCollectorFile.h" // Only to get maximum frame rate in file mode
#include "vtkDataCollectorHardwareDevice.h" // Only to get maximum frame rate in device mode
#include "vtkPlusVideoSource.h" // Only to get maximum frame rate in device mode
#include "vtkTracker.h" // Only to get maximum frequency in device mode

#include <QFileDialog>
#include <QMessageBox>
#include <QTimer>

#include "vtkTrackedFrameList.h"
#include "TrackedFrame.h"

#include "vtksys/SystemTools.hxx"

//-----------------------------------------------------------------------------

CapturingToolbox::CapturingToolbox(fCalMainWindow* aParentMainWindow, Qt::WFlags aFlags)
: AbstractToolbox(aParentMainWindow)
, QWidget(aParentMainWindow, aFlags)
, m_RecordedFrames(NULL)
, m_RecordingTimer(NULL)
, m_LastRecordedFrameTimestamp(0.0)
, m_SamplingFrameRate(8)
, m_RequestedFrameRate(0.0)
, m_ActualFrameRate(0.0)
{
  ui.setupUi(this);

  // Create tracked frame list
  m_RecordedFrames = vtkTrackedFrameList::New();
  m_RecordedFrames->SetValidationRequirements(REQUIRE_UNIQUE_TIMESTAMP); 

  // Connect events
  connect( ui.pushButton_Snapshot, SIGNAL( clicked() ), this, SLOT( TakeSnapshot() ) );
  connect( ui.pushButton_Record, SIGNAL( clicked() ), this, SLOT( Record() ) );
  connect( ui.pushButton_ClearRecordedFrames, SIGNAL( clicked() ), this, SLOT( ClearRecordedFrames() ) );
  connect( ui.pushButton_Save, SIGNAL( clicked() ), this, SLOT( Save() ) );

  connect( ui.horizontalSlider_SamplingRate, SIGNAL( valueChanged(int) ), this, SLOT( SamplingRateChanged(int) ) );

  // Create and connect recording timer
  m_RecordingTimer = new QTimer(this); 
  connect(m_RecordingTimer, SIGNAL(timeout()), this, SLOT(Capture()) );
}

//-----------------------------------------------------------------------------

CapturingToolbox::~CapturingToolbox()
{
  if (m_RecordedFrames != NULL) {
    m_RecordedFrames->Delete();
    m_RecordedFrames = NULL;
  }
}

//-----------------------------------------------------------------------------

void CapturingToolbox::Initialize()
{
  LOG_TRACE("CapturingToolbox::Initialize"); 

  if ((m_ParentMainWindow->GetObjectVisualizer()->GetDataCollector() != NULL) && (m_ParentMainWindow->GetObjectVisualizer()->GetDataCollector()->GetConnected()))
  {
    //m_ParentMainWindow->GetObjectVisualizer()->GetDataCollector()->SetTrackingOnly(false);

    // Set initialized if it was uninitialized
    if (m_State == ToolboxState_Uninitialized)
    {
      SetState(ToolboxState_Idle);
    }
    else
    {
      SetDisplayAccordingToState();
    }
  }
  else
  {
    SetState(ToolboxState_Uninitialized);
  }
}

//-----------------------------------------------------------------------------

void CapturingToolbox::RefreshContent()
{
  //LOG_TRACE("CapturingToolbox::RefreshContent");

  if (m_State == ToolboxState_InProgress)
  {
    ui.label_ActualRecordingFrameRate->setText(QString::number(m_ActualFrameRate, 'f', 2));
    ui.label_NumberOfRecordedFrames->setText(QString::number(m_RecordedFrames->GetNumberOfTrackedFrames()));
  }
}

//-----------------------------------------------------------------------------

void CapturingToolbox::SetDisplayAccordingToState()
{
  LOG_TRACE("CapturingToolbox::SetDisplayAccordingToState");

  // If the force show devices isn't enabled, set it to 2D
  if( !m_ParentMainWindow->IsForceShowDevicesEnabled() )
  {
    m_ParentMainWindow->GetObjectVisualizer()->SetVisualizationMode(vtkVisualizationController::DISPLAY_MODE_2D);
  }

  // Enable or disable the image manipulation menu
  m_ParentMainWindow->SetImageManipulationEnabled( m_ParentMainWindow->GetObjectVisualizer()->Is2DMode() );

  // Hide or show the orientation markers based on the value of the checkbox
  m_ParentMainWindow->GetObjectVisualizer()->ShowOrientationMarkers(m_ParentMainWindow->IsOrientationMarkersEnabled());

  if (m_State == ToolboxState_Uninitialized)
  {
    ui.pushButton_Snapshot->setEnabled(false);
    ui.pushButton_Record->setEnabled(false);
    ui.pushButton_ClearRecordedFrames->setEnabled(false);
    ui.pushButton_Save->setEnabled(false);
    ui.horizontalSlider_SamplingRate->setEnabled(false);
  }
  else if (m_State == ToolboxState_Idle)
  {
    ui.pushButton_Record->setText("Record");
    ui.pushButton_Record->setIcon( QPixmap( ":/icons/Resources/icon_Record.png" ) );

    ui.pushButton_Snapshot->setEnabled(true);
    ui.pushButton_Record->setEnabled(true);
    ui.pushButton_ClearRecordedFrames->setEnabled(false);
    ui.pushButton_Save->setEnabled(false);
    ui.horizontalSlider_SamplingRate->setEnabled(true);

    SamplingRateChanged(ui.horizontalSlider_SamplingRate->value());

    ui.label_ActualRecordingFrameRate->setText(tr("0.0"));
    ui.label_MaximumRecordingFrameRate->setText(QString::number( GetMaximumFrameRate() ));

    ui.label_NumberOfRecordedFrames->setText("0");
  }
  else if (m_State == ToolboxState_InProgress)
  {
    ui.pushButton_Record->setText(tr("Stop"));
    ui.pushButton_Record->setIcon( QPixmap( ":/icons/Resources/icon_Stop.png" ) );

    ui.pushButton_Snapshot->setEnabled(false);
    ui.pushButton_Record->setEnabled(true);
    ui.pushButton_ClearRecordedFrames->setEnabled(false);
    ui.pushButton_Save->setEnabled(false);
    ui.horizontalSlider_SamplingRate->setEnabled(false);

    // Change the function to be invoked on clicking on the now Stop button
    disconnect( ui.pushButton_Record, SIGNAL( clicked() ), this, SLOT( Record() ) );
    connect( ui.pushButton_Record, SIGNAL( clicked() ), this, SLOT( Stop() ) );
  }
  else if (m_State == ToolboxState_Done)
  {
    ui.pushButton_Record->setText(tr("Record"));
    ui.pushButton_Record->setIcon( QIcon( ":/icons/Resources/icon_Record.png" ) );

    ui.pushButton_Snapshot->setEnabled(true);
    ui.pushButton_Record->setEnabled(true);
    ui.pushButton_ClearRecordedFrames->setEnabled(true);
    ui.pushButton_Save->setEnabled(true);
    ui.horizontalSlider_SamplingRate->setEnabled(true);

    ui.label_ActualRecordingFrameRate->setText(tr("0.0"));
    ui.label_MaximumRecordingFrameRate->setText(QString::number( GetMaximumFrameRate() ));

    ui.label_NumberOfRecordedFrames->setText(QString::number(m_RecordedFrames->GetNumberOfTrackedFrames()));

    // Change the function to be invoked on clicking on the now Record button
    disconnect( ui.pushButton_Record, SIGNAL( clicked() ), this, SLOT( Stop() ) );
    connect( ui.pushButton_Record, SIGNAL( clicked() ), this, SLOT( Record() ) );
  }
  else if (m_State == ToolboxState_Error)
  {
    ui.pushButton_Snapshot->setEnabled(false);
    ui.pushButton_Record->setEnabled(false);
    ui.pushButton_ClearRecordedFrames->setEnabled(false);
    ui.pushButton_Save->setEnabled(false);
    ui.horizontalSlider_SamplingRate->setEnabled(false);
  }
}

//-----------------------------------------------------------------------------

void CapturingToolbox::TakeSnapshot()
{
  LOG_TRACE("CapturingToolbox::TakeSnapshot"); 

  TrackedFrame trackedFrame;

  if (m_ParentMainWindow->GetObjectVisualizer()->GetDataCollector()->GetTrackedFrame(&trackedFrame) != PLUS_SUCCESS)
  {
    LOG_ERROR("Failed to get tracked frame for the snapshot!");
    return;
  }

  // Check if there are any valid transforms
  std::vector<PlusTransformName> transformNames;
  trackedFrame.GetCustomFrameTransformNameList(transformNames);
  bool validFrame = false;

  if (transformNames.size() == 0)
  {
    validFrame = true;
  }
  else
  {
    for (std::vector<PlusTransformName>::iterator it = transformNames.begin(); it != transformNames.end(); ++it)
    {
      TrackedFrameFieldStatus status = FIELD_INVALID;
      trackedFrame.GetCustomFrameTransformStatus(*it, status);

      if ( status == FIELD_OK )
      {
        validFrame = true;
        break;
      }
    }
  }

  if ( !validFrame )
  {
    LOG_WARNING("Unable to record tracked frame: All the tool transforms are invalid!"); 
    return;
  }

  // Add tracked frame to the list
  if (m_RecordedFrames->AddTrackedFrame(&trackedFrame, vtkTrackedFrameList::SKIP_INVALID_FRAME) != PLUS_SUCCESS)
  {
    LOG_WARNING("Frame could not be added because validation failed!");
    return;
  }

  SetState(ToolboxState_Done);

  LOG_INFO("Snapshot taken");
}

//-----------------------------------------------------------------------------

void CapturingToolbox::Record()
{
  LOG_INFO("Capturing started");

  m_ParentMainWindow->SetTabsEnabled(false);

  // Reset accessory members
  m_RecordedFrameNumberQueue.clear();

  vtkDataCollector* dataCollector = NULL;
  if ( (m_ParentMainWindow == NULL) || (m_ParentMainWindow->GetObjectVisualizer() == NULL) || ((dataCollector = m_ParentMainWindow->GetObjectVisualizer()->GetDataCollector()) == NULL) )
  {
    LOG_ERROR("Unable to reach valid data collector object!");
    return;
  }

  dataCollector->GetMostRecentTimestamp(m_LastRecordedFrameTimestamp);

  // Start capturing
  SetState(ToolboxState_InProgress);
  m_RecordingTimer->start(1000.0 / m_SamplingFrameRate); 
}

//-----------------------------------------------------------------------------

void CapturingToolbox::Capture()
{
  //LOG_TRACE("CapturingToolbox::Capture");

  double startTimeSec = vtkAccurateTimer::GetSystemTime();

  vtkDataCollector* dataCollector = NULL;
  if ( (m_ParentMainWindow == NULL) || (m_ParentMainWindow->GetObjectVisualizer() == NULL) || ((dataCollector = m_ParentMainWindow->GetObjectVisualizer()->GetDataCollector()) == NULL) )
  {
    LOG_ERROR("Unable to reach valid data collector object!");
    return;
  }

  int numberOfFramesBeforeRecording = m_RecordedFrames->GetNumberOfTrackedFrames();

  // Record
  if ( dataCollector->GetTrackedFrameListSampled(m_LastRecordedFrameTimestamp, m_RecordedFrames, 1.0 / m_RequestedFrameRate) != PLUS_SUCCESS )
  {
    LOG_ERROR("Failed to get tracked frame list from data collector (last recorded timestamp: " << std::fixed << m_LastRecordedFrameTimestamp ); 
    return; 
  }

  int numberOfRecordedFrames = m_RecordedFrames->GetNumberOfTrackedFrames() - numberOfFramesBeforeRecording;

  // Compute actual frame rate
  m_RecordedFrameNumberQueue.push_back(numberOfRecordedFrames);

  while (m_RecordedFrameNumberQueue.size() > m_SamplingFrameRate * 2)
  {
    m_RecordedFrameNumberQueue.pop_front();
  }

  int numberOfRecordedFramesInTheLastTwoSeconds = 0;
  for (std::deque<int>::iterator it = m_RecordedFrameNumberQueue.begin(); it != m_RecordedFrameNumberQueue.end(); ++it)
  {
    numberOfRecordedFramesInTheLastTwoSeconds += (*it);
  }

  m_ActualFrameRate = (double)numberOfRecordedFramesInTheLastTwoSeconds / (m_RecordedFrameNumberQueue.size() * (1.0 / m_SamplingFrameRate));

  // Check whether the recording needed more time than the sampling interval
  double recordingTimeMs = (vtkAccurateTimer::GetSystemTime() - startTimeSec) * 1000.0;
  if (recordingTimeMs > 1000.0 / m_SamplingFrameRate)
  {
    LOG_WARNING("Recording cannot keep up with aquisition!");
  }
}

//-----------------------------------------------------------------------------

void CapturingToolbox::Stop()
{
  LOG_INFO("Capturing stopped");

  m_RecordingTimer->stop();
  SetState(ToolboxState_Done);

  m_ParentMainWindow->SetTabsEnabled(true);
}

//-----------------------------------------------------------------------------

void CapturingToolbox::Save()
{
  LOG_TRACE("CapturingToolbox::Save"); 

  QString filter = QString( tr( "SequenceMetaFiles (*.mha *.mhd);;" ) );
  QString fileName = QFileDialog::getSaveFileName(NULL, tr("Save captured tracked frames"), QString(vtkPlusConfig::GetInstance()->GetImageDirectory()), filter);

  if (! fileName.isNull() )
  {
    QApplication::setOverrideCursor(QCursor(Qt::BusyCursor));

    // Actual saving
    std::string filePath = fileName.ascii();
    std::string path = vtksys::SystemTools::GetFilenamePath(filePath); 
    std::string filename = vtksys::SystemTools::GetFilenameWithoutExtension(filePath); 
    std::string extension = vtksys::SystemTools::GetFilenameExtension(filePath); 

    vtkTrackedFrameList::SEQ_METAFILE_EXTENSION ext(vtkTrackedFrameList::SEQ_METAFILE_MHA); 
    if ( STRCASECMP(".mhd", extension.c_str() ) == 0 )
    {
      ext = vtkTrackedFrameList::SEQ_METAFILE_MHD; 
    }
    else
    {
      ext = vtkTrackedFrameList::SEQ_METAFILE_MHA; 
    }

    if ( m_RecordedFrames->SaveToSequenceMetafile(path.c_str(), filename.c_str(), ext, false) != PLUS_SUCCESS )
    {
      LOG_ERROR("Failed to save tracked frames to sequence metafile!"); 
      return;
    }

    // Add file name to image list in Volume reconstruction toolbox
    VolumeReconstructionToolbox* volumeReconstructionToolbox = dynamic_cast<VolumeReconstructionToolbox*>(m_ParentMainWindow->GetToolbox(ToolboxType_VolumeReconstruction));
    if (volumeReconstructionToolbox != NULL)
    {
      volumeReconstructionToolbox->AddImageFileName(fileName);
    }

    // Save config file next to the tracked frame list
    std::string configFileName = path + "/" + filename + "_config.xml";
    PlusCommon::PrintXML(configFileName.c_str(), vtkPlusConfig::GetInstance()->GetDeviceSetConfigurationData());

    m_RecordedFrames->Clear(); 
    SetState(ToolboxState_Idle);

    QApplication::restoreOverrideCursor();
  }	

  LOG_INFO("Captured tracked frame list saved into '" << fileName.toAscii().data() << "'");
}

//-----------------------------------------------------------------------------

void CapturingToolbox::ClearRecordedFrames()
{
  LOG_TRACE("CapturingToolbox::ClearRecordedFrames"); 

  if ( QMessageBox::question(this, tr("fCal - Capturing"), tr("Do you want to discard all the recorded frames?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::No )
  {
    return; 
  }

  m_RecordedFrames->Clear();

  SetState(ToolboxState_Idle);

  LOG_INFO("Recorded frames cleared");
}

//-----------------------------------------------------------------------------

void CapturingToolbox::SamplingRateChanged(int aValue)
{
  LOG_TRACE("CapturingToolbox::RecordingFrameRateChanged(" << aValue << ")"); 

  double maxFrameRate = GetMaximumFrameRate();
  int samplingRate = (int)(pow(2.0, ui.horizontalSlider_SamplingRate->maxValue() - aValue));

  m_RequestedFrameRate = maxFrameRate / (double)samplingRate;

  ui.horizontalSlider_SamplingRate->setToolTip(tr("1 / ").append(QString::number((int)samplingRate)));
  ui.label_RequestedRecordingFrameRate->setText(QString::number(m_RequestedFrameRate, 'f', 2));

  LOG_INFO("Sampling rate changed to " << aValue << " (matching requested frame rate is " << m_RequestedFrameRate << ")");
}

//-----------------------------------------------------------------------------

double CapturingToolbox::GetMaximumFrameRate()
{
  LOG_TRACE("CapturingToolbox::GetMaximumFrameRate");

  if (m_ParentMainWindow == NULL || m_ParentMainWindow->GetObjectVisualizer() == NULL || m_ParentMainWindow->GetObjectVisualizer()->GetDataCollector() == NULL)
  {
    LOG_ERROR("Unable to reach valid data collector object!");
    return 0.0;
  }

  double frameRate = 0.0;
  if (m_ParentMainWindow->GetObjectVisualizer()->GetDataCollector()->GetFrameRate(frameRate)  != PLUS_SUCCESS)
  {
    LOG_ERROR("Unable to get frame rate from data collector!");
  }

  return frameRate;
}
